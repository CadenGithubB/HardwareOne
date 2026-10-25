#!/usr/bin/env python3
"""Execute the glasses' device open/close on top of the shared BLE central stack.

initG2Client / deinitG2Client open and close the glasses runtime only. The
shared central stack (BLE_CentralStack.cpp: Bluedroid host, controller link,
setup worker, scan object, GAP handler, the app console's service on the client
host, peer registration and auto-reconnect arming) has its own owner, and the
glasses meet it only through bleStackUp(), bleStackScan() and
bleStackLastRefusal(). The stack, temples, workers, heap samples and timers are
host boundaries; production lifecycle claims, init/refusal/teardown control
flow, runtime state, combined status snapshots, page/tap worker admission, the
lazy worker-creation gate with its admission flag (and that flag's boot value),
the memory checker and the "g2client" row are extracted unchanged. No host, pipe, ring or console function has a fake, so
production that calls one does not compile here.

Source checks pin the order inside both functions and keep the host, pipe,
ring and console calls out of them; hold the console's command service to its
one caller, the stack owner; and require each lazy worker creator to consult
the creation gate inside its own critical section before it claims its slot.
Concurrency exercises production publication and read helpers with real host
mutexes, without claiming telemetry atomicity or hardware allocation fidelity.
The harness builds with and without the phone pipe and requires the same
outcomes: the glasses lifecycle no longer depends on it.
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

# Calls that belong to the stack, the phone pipe, the ring or the console
# owner. The glasses open/close must not make them (the harness has no fakes
# for them either, so a reintroduced call also fails to compile).
FOREIGN_CALLS = ("phonePipeStop(", "phonePipeStart(", "g2RingInit(", "g2RingDisconnect(",
                 "bluetoothHalInit(", "bluetoothHalDeinit(", "deinitBluetooth(",
                 "bleArmAutoReconnectPeers(", "bleConsoleRegisterOnClientHost(", "bleStackDown(")
LIFECYCLE_ENTER = "portENTER_CRITICAL(&gControlLifecycleMux)"
LIFECYCLE_EXIT = "portEXIT_CRITICAL(&gControlLifecycleMux)"


def generated(component: Path) -> str:
    source = (component / "G2_Glasses.cpp").read_text()
    header = (component / "G2_Glasses.h").read_text()
    memory = (component / "System_MemoryMonitor.cpp").read_text()
    memory_header = (component / "System_MemoryMonitor.h").read_text()
    requirement = re.search(r'\{\s*"g2client"\s*,[^}]+\}', memory)
    require(requirement is not None, "production G2 memory registry entry is present")
    # The creation gate's admission flag, with its boot value.
    admission = re.search(r"^static bool gClientUiCreationOpen\b[^;]*;", source, re.M)
    require(admission is not None, "the UI-worker creation gate's admission flag is present")
    types = [extract_block(memory_header, "struct MemoryRequirement") + ";",
             "static const MemoryRequirement gMemoryRequirements[] = {" + requirement[0] + "};",
             "static const size_t gMemoryRequirementsCount = 1;", admission[0]]
    types += [extract_block(header, marker) + ";" for marker in (
        "enum G2State", "enum G2HijackPage",
        "struct G2TempleConnectionStatus", "struct G2ConnectionStatus")]
    types += [extract_block(source, marker) + ";" for marker in (
        "struct G2LinkSnapshot", "enum class G2WorkerInitState",
        "struct G2ClientRuntime", "struct G2ClientStatusSnapshot")]
    definitions = [extract_block(memory, marker) for marker in (
        "const MemoryRequirement* getMemoryRequirement(", "bool checkMemoryAvailable(")]
    definitions += [extract_block(source, marker) for marker in (
        "static G2LinkSnapshot g2LinkSnapshot(",
        "static G2ClientRuntime g2ClientRuntimeSnapshot(",
        "static void g2ClientRuntimeReset(",
        "static void g2ClientSetState(",
        "static void g2ClientConnected(",
        "static G2ClientStatusSnapshot g2ClientStatusSnapshotLocked(",
        "static G2ClientStatusSnapshot g2ClientStatusSnapshot(",
        "static const char* g2ClientStateName(",
        "static bool g2RoleAdmissionOpen(",
        "static void g2ControlStartBlockAcquire(",
        "static void g2ControlStartBlockRelease(",
        "static bool g2ClientInitBegin(",
        "static void g2ClientInitDone(",
        "static bool g2ClientInitIdle(",
        # The harness's worker creators consult the production gate.
        "static bool g2UiWorkerCreationOpenLocked(",
        "static void g2UiSubmitDone(")]
    definitions += [extract_block(source, marker) + ";" for marker in (
        "struct G2ClientInitClaim", "class G2BleRoleTransitionClaim")]
    definitions += [extract_block(source, marker) for marker in (
        "bool initG2Client(", "bool deinitG2Client(",
        "bool isG2ClientInitialized(", "G2State getG2State(",
        "const char* getG2StateString(", "bool g2ConnectionStatusSnapshot(",
        "static bool pageSwapRuntimeSnapshot(", "static bool tapRuntimeSnapshot(")]
    harness = (HERE / "g2_client_lifecycle_harness.cpp").read_text()
    return harness.replace("// INSERT_TYPES", "\n".join(types)).replace(
        "// INSERT_PRODUCTION", "\n\n".join(definitions))


def code_only(text: str) -> str:
    """Blank out // and /* */ comments; keep string literals and offsets."""
    out, pos, state = [], 0, "code"
    while pos < len(text):
        char, following = text[pos], text[pos + 1:pos + 2]
        if state == "line":
            out.append(char if char == "\n" else " ")
            if char == "\n":
                state = "code"
        elif state == "block":
            if char == "*" and following == "/":
                out.append("  ")
                pos += 2
                state = "code"
                continue
            out.append(char if char == "\n" else " ")
        elif state in ("string", "character"):
            out.append(char)
            if char == "\\":
                out.append(following)
                pos += 2
                continue
            if char == ('"' if state == "string" else "'"):
                state = "code"
        elif char == "/" and following in ("/", "*"):
            state = "line" if following == "/" else "block"
            out.append("  ")
            pos += 2
            continue
        else:
            if char in "\"'":
                state = "string" if char == '"' else "character"
            out.append(char)
        pos += 1
    return "".join(out)


def drop_calls(code: str, name: str) -> str:
    """Remove every `name(...);` statement, matching parentheses outside literals."""
    out, pos = [], 0
    while (start := code.find(name + "(", pos)) != -1:
        out.append(code[pos:start])
        index, depth, quote = start + len(name), 0, ""
        while True:
            char = code[index]
            if quote:
                if char == "\\":
                    index += 1
                elif char == quote:
                    quote = ""
            elif char in "\"'":
                quote = char
            elif char == "(":
                depth += 1
            elif char == ")":
                depth -= 1
                if depth == 0:
                    break
            index += 1
        semicolon = re.match(r"\s*;", code[index + 1:])
        pos = index + 1 + (semicolon.end() if semicolon else 0)
    return "".join(out) + code[pos:]


def once(code: str, call: str, where: str) -> int:
    require(code.count(call) == 1, f"{where} calls {call} exactly once")
    return code.index(call)


def in_order(code: str, calls: tuple[str, ...], where: str) -> None:
    previous, at = None, -1
    for call in calls:
        index = once(code, call, where)
        require(index > at, f"{where}: {call} comes after {previous}")
        previous, at = call, index


def check_lifecycle_order(component: Path) -> None:
    """initG2Client opens the runtime on the shared stack; deinitG2Client closes
    the runtime only and leaves the stack, the pipe, the ring and the console
    to their owners."""
    source = (component / "G2_Glasses.cpp").read_text()
    init = code_only(extract_block(source, "bool initG2Client("))
    stop = code_only(extract_block(source, "bool deinitG2Client("))

    stack = once(init, "bleStackUp()", "initG2Client")
    require(stack < init.index("templeRuntimeQuiescedForInit("),
            "initG2Client brings the shared stack up before the temple barrier")
    memory = once(init, 'checkMemoryAvailable("g2client"', "initG2Client")
    require(stack < memory < init.index("templeInit("),
            "the runtime's memory gate follows the stack and precedes every temple")
    require(once(init, "g2ClientRuntimeReset(true)", "initG2Client") <
            once(init, "g2RegisterBlePeer();", "initG2Client"),
            "the glasses' peer registers on the published runtime")
    session = "g2SessionEnsureWorker()"
    tail = init[once(init, session, "initG2Client") + len(session):]
    require(re.fullmatch(r"\s*;\s*return true;\s*\}\s*", drop_calls(tail, "DEBUG_G2F")) is not None,
            "the session worker is the last step before initG2Client returns true")

    in_order(stop, ("g2SessionShutdown()", "g2UiWorkersQuiesce(", "g2Disconnect()",
                    "g2NativeNotesRelease(",
                    "bleSetupWaitIdle(hw1setup::clientMask(hw1setup::Client::G2)",
                    "templeReset(gL)", "g2NotifyClearTimerStop()",
                    "g2SetHijackPage(G2_HIJACK_PAGE_MAIN)", "g2ClientRuntimeReset()",
                    "g2UiWorkersRelease("), "deinitG2Client")
    require(stop.count("bleSetupWaitIdle(") == 1 and "kAccessories" not in stop,
            "deinitG2Client waits only for the glasses' own setup slot")
    require(stop.index("g2UiWorkersRelease(") < stop.rindex("g2ControlStartBlockRelease()"),
            "the UI workers are released while the stop still holds the start block")

    for name, body in (("initG2Client", init), ("deinitG2Client", stop)):
        for call in FOREIGN_CALLS:
            require(call not in body, f"{name} must not call {call[:-1]}: it has its own owner")


def check_console_single_caller(component: Path) -> None:
    """The app console's command service registers in bleStackUp() on a fresh
    client host, and nowhere else, so no service starts while links are up."""
    call = "bleConsoleRegisterOnClientHost("
    users = {}
    for path in sorted(component.rglob("*")):
        relative = path.relative_to(component)
        if relative.parts[0] == "test" or path.suffix not in (".cpp", ".inc"):
            continue
        text = path.read_text(errors="replace")
        if call in text and (count := code_only(text).count(call)):
            users[relative.as_posix()] = count
    require(users == {"BLE_CentralStack.cpp": 1, "Bluetooth.cpp": 1},
            f"the stack owner is the console registration's only caller: {users}")
    require("bool bleConsoleRegisterOnClientHost() {" in
            code_only((component / "Bluetooth.cpp").read_text()),
            "Bluetooth.cpp holds the registration's definition, not a call")


def check_worker_creation_gate(component: Path) -> None:
    """Each lazy UI-worker creator refuses a closed gate and claims its slot in
    the same lifecycle critical section, before it allocates anything; the
    gate's admission flag is written only under that lock."""
    source = (component / "G2_Glasses.cpp").read_text()
    code = code_only(source)
    writes = [found.start() for found in
              re.finditer(r"(?<!static bool )\bgClientUiCreationOpen\s*=(?!=)", code)]
    require(bool(writes), "the admission flag is written somewhere")
    for at in writes:
        entered = code.rfind(LIFECYCLE_ENTER, 0, at)
        require(entered != -1 and LIFECYCLE_EXIT not in code[entered:at],
                f"line {code.count(chr(10), 0, at) + 1} writes the admission flag "
                "without holding gControlLifecycleMux")
    gate = "g2UiWorkerCreationOpenLocked()"
    for creator in ("pageSwapInit", "tapDispatcherInit", "g2SessionEnsureWorker"):
        # The definition, not the forward declaration earlier in the file.
        signature = f"static bool {creator}()"
        found = re.search(re.escape(signature) + r"\s*\{", source)
        require(found is not None, f"{creator} is defined in G2_Glasses.cpp")
        body = code_only(extract_block(source[found.start():], signature))
        # Early returns release the lock on their own branch; what remains is
        # the path that reaches the gate and the slot claim.
        path = re.sub(re.escape(LIFECYCLE_EXIT) + r";\s*return [^;]*;", "", body)
        at = once(path, gate, creator)
        require(path[:at].rstrip().endswith("!"), f"{creator} refuses when the gate is closed")
        entered = path.rfind(LIFECYCLE_ENTER, 0, at)
        require(entered != -1 and LIFECYCLE_EXIT not in path[entered:at],
                f"{creator} consults the gate inside its lifecycle critical section")
        claim = path.find("InitState = G2WorkerInitState::Starting;", at)
        require(claim != -1 and LIFECYCLE_EXIT not in path[at:claim],
                f"{creator} claims its slot in the critical section that checked the gate")
        require(claim < path.index("xQueueCreate("), f"{creator} allocates only after the gate")


def check_stale_client_retire_before_scan(component: Path) -> None:
    """A temple's stale BLEClient (set by onDisconnect) is retired before every
    connect job scans, not only when its own temple advertises again
    (memory audit BAPP-01); connectTemple keeps the late catch through the same
    helper, the only place the flag is cleared by retirement."""
    source = (component / "G2_Glasses.cpp").read_text()
    # The helpers are forward-declared beside connectTemple's declaration, so
    # each block is taken from its definition, not its first mention.
    def definition(signature: str) -> str:
        return code_only(extract_block(source[source.index(signature + " {"):], signature))
    helper = definition("static bool g2RetireStaleTempleClient(G2Temple& t)")
    in_order(helper, ("if (!t.client || !t.clientStale) return true;",
                      "xSemaphoreTake(t.writeMutex, pdMS_TO_TICKS(2000))",
                      "bleRetireClientForReplacement(", "xSemaphoreGive(t.writeMutex);",
                      "if (!retired) return false;", "t.clientStale = false;"),
             "g2RetireStaleTempleClient")
    both = definition("static void g2RetireStaleTempleClients()")
    require("g2RetireStaleTempleClient(gL)" in both and "g2RetireStaleTempleClient(gR)" in both,
            "g2RetireStaleTempleClients covers both temples")
    for job in ("static bool g2ConnectSync(", "static bool attemptMissingArmRecoverySync("):
        body = code_only(extract_block(source, job))
        require(once(body, "g2RetireStaleTempleClients();", job) <
                body.index("gScan->setAdvertisedDeviceCallbacks("),
                f"{job} retires stale temple clients before its scan")
    connect = definition("static bool connectTemple(G2Temple& t, hw1coex::Purpose purpose)")
    require("if (!g2RetireStaleTempleClient(t)) return false;" in connect and
            "bleRetireClientForReplacement(" not in connect,
            "connectTemple retires through the helper only")
    require(source.count("clientStale = false;") == 2,
            "the stale flag is cleared by the helper and by a fresh client only")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--thread-sanitize", action="store_true")
    parser.add_argument("--component", type=Path, default=COMPONENT,
                        help="read production from another copy of components/hardwareone "
                             "(mutation runs)")
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    require(not (args.sanitize and args.thread_sanitize), "select one sanitizer mode")
    flags = ["-std=c++17", "-pthread", "-Wall", "-Wextra", "-Werror", "-pedantic",
             "-Wno-unused-function", "-Wno-unused-variable", "-Wno-unused-parameter"]
    if args.sanitize or args.thread_sanitize:
        flags += ["-fsanitize=" + ("thread" if args.thread_sanitize else "address,undefined"),
                  "-fno-omit-frame-pointer", "-g"]
    check_lifecycle_order(args.component)
    check_console_single_caller(args.component)
    check_worker_creation_gate(args.component)
    check_stale_client_retire_before_scan(args.component)
    with tempfile.TemporaryDirectory(prefix="hw1-g2-lifecycle-") as directory:
        work = Path(directory)
        source = generated(args.component)

        def run(name: str, pipe: int) -> str:
            path = work / (name + ".cpp")
            path.write_text(source)
            subprocess.run([args.cxx, *flags, f"-DENABLE_BLE_PHONE_PIPE={pipe}", str(path),
                            "-o", str(work / name)], check=True, timeout=120)
            return subprocess.check_output([str(work / name)], text=True, timeout=120)

        require(run("current-no-pipe", 0) == run("current", 1),
                "lifecycle outcomes differ between pipe and no-pipe builds")
    print("G2 lifecycle order: stack first, runtime gate before temples and workers, session "
          "worker last; the stop closes the runtime only; no host, pipe, ring or console call")
    print("G2 memory admission: production threshold on the running stack, restart, kept-worker "
          "reuse and refusal precedence passed")
    print("G2 lifecycle: production transitions, worker-creation gate, guarded publication and "
          "concurrent readers passed")


if __name__ == "__main__":
    main()
