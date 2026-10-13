#!/usr/bin/env python3
"""Source-contract guards for single-resolution command execution."""

from pathlib import Path
import re


COMPONENT = Path(__file__).resolve().parents[2]


def function_body(source: str, signature: str) -> str:
    """Return a C++ function body while ignoring braces in comments/quotes."""

    start = source.index(signature)
    pos = source.index("{", start)
    brace = pos
    depth = 0
    state = "code"
    while pos < len(source):
        current = source[pos]
        following = source[pos + 1] if pos + 1 < len(source) else ""

        if state == "line_comment":
            if current == "\n":
                state = "code"
        elif state == "block_comment":
            if current == "*" and following == "/":
                state = "code"
                pos += 1
        elif state == "string":
            if current == "\\":
                pos += 1
            elif current == '"':
                state = "code"
        elif state == "character":
            if current == "\\":
                pos += 1
            elif current == "'":
                state = "code"
        else:
            if current == "/" and following == "/":
                state = "line_comment"
                pos += 1
            elif current == "/" and following == "*":
                state = "block_comment"
                pos += 1
            elif current == '"':
                state = "string"
            elif current == "'":
                state = "character"
            elif current == "{":
                depth += 1
            elif current == "}":
                depth -= 1
                if depth == 0:
                    return source[brace : pos + 1]
        pos += 1
    raise AssertionError(f"unterminated function: {signature}")


command_h = (COMPONENT / "System_Command.h").read_text(encoding="utf-8")
command_cpp = (COMPONENT / "System_Command.cpp").read_text(encoding="utf-8")
lookup_h = (COMPONENT / "System_CommandLookupCore.h").read_text(encoding="utf-8")
utils_h = (COMPONENT / "System_Utils.h").read_text(encoding="utf-8")
utils_cpp = (COMPONENT / "System_Utils.cpp").read_text(encoding="utf-8")

# The hot resolver stays dependency-free and allocation-free. Registration
# order remains the tie-breaker; no stale index/cache is introduced around the
# boot-populated registry.
assert "#include <Arduino" not in lookup_h
assert "String" not in lookup_h
assert "new " not in lookup_h
assert "malloc(" not in lookup_h
assert "nameLength <= result.matchedLength" in lookup_h
assert "isCommandSpace(trimmed[nameLength])" in lookup_h
assert "CommandResolution resolveCommand(const String& line);" in command_h

resolve = function_body(command_cpp, "CommandResolution resolveCommand(")
assert resolve.count("hw1_command_lookup::resolve(") == 1
assert "commandRegistrySize" in resolve
assert "commandRegistry[index]" in resolve
assert "String(" not in resolve

# All compatibility entrypoints still work, but delegate to the shared result.
legacy_find = function_body(command_cpp, "const CommandEntry* findCommand(")
assert "resolveCommand(cmdLine).entry" in legacy_find
canonical = function_body(command_cpp, "String resolveRegistryCommandKey(")
assert canonical.count("resolveCommand(command)") == 1

public_redactor = function_body(
    utils_cpp, "String redactCmdForAudit(const String& argsInput)"
)
assert public_redactor.count("resolveCommand(argsInput)") == 1
assert "redactCmdForAudit(argsInput, resolution)" in public_redactor
resolved_redactor = function_body(
    utils_cpp,
    "String redactCmdForAudit(const String& argsInput,\n"
    "                         const CommandResolution& resolution)",
)
assert "!resolution.entry" in resolved_redactor
assert "findCommand(" not in resolved_redactor
assert "CommandResolution& resolution" in utils_h
assert 'return c.substring(0, verbEnd) + " ***";' in resolved_redactor

# The dependency-light registry path resolves once and dispatches that exact
# row. In particular, it never canonicalizes to text and performs a second
# registry scan.
direct = function_body(command_cpp, "String executeCommandThroughRegistry(")
assert direct.count("resolveCommand(command)") == 1
assert "const CommandEntry* found = resolution.entry;" in direct
assert "resolution.matchedLength" in direct
assert "resolveRegistryCommandKey(" not in direct
assert "commandRegistry[" not in direct

# Authorization consumes the caller's result and already-redacted line. This
# keeps super-implies-admin behavior without either helper doing another scan.
authorize = function_body(utils_cpp, "static bool authorizeCommand(")
authorize_signature = utils_cpp[
    utils_cpp.index("static bool authorizeCommand(") :
    utils_cpp.index("{", utils_cpp.index("static bool authorizeCommand("))
]
assert "const CommandResolution& resolution" in authorize_signature
assert "const String& redactedLine" in authorize_signature
assert "resolution.entry->requiresSuperAdmin" in authorize
assert "resolution.entry->requiresAdmin" in authorize
assert "findCommand(" not in authorize
assert "redactCmdForAudit(" not in authorize
# The row flags are read in authorizeCommand only: the second-scan helpers
# (commandRequiresAdmin/SuperAdmin) and the auth-less dispatchCommand are gone.
for removed in ("commandRequiresAdmin(", "commandRequiresSuperAdmin(", "dispatchCommand("):
    assert removed not in utils_cpp, removed
    assert removed not in command_cpp, removed

# The normal local executor has exactly one outer resolution, reused for
# redaction, authorization, dispatch, unknown output, command audit, and auth
# audit. The remote wrapper deliberately resolves its distinct inner command
# only after nested-wrapper rejection and authorizes that inner row separately.
execute = function_body(utils_cpp, "bool executeCommand(AuthContext& ctx,")
assert execute.count("resolveCommand(command)") == 1
assert execute.count("resolveCommand(actualCommand)") == 1
assert "redactCmdForAudit(command, commandResolution)" in execute
assert "authorizeCommand(ctx, command, commandResolution," in execute
assert "const CommandEntry* found = commandResolution.entry;" in execute
assert "const size_t foundLen = commandResolution.matchedLength;" in execute
assert "findCommand(command)" not in execute
assert "findCommand(actualCommand)" not in execute
assert execute.count("logCommandExecutionPrepared(") == 2
assert "safeCommandForTrace.c_str()" in execute

nested_rejection = execute.index("inner.startsWith(\"remote:\")")
remote_resolution = execute.index(
    "const CommandResolution remoteResolution = resolveCommand(actualCommand);"
)
remote_authorize = execute.index(
    "authorizeCommand(ctx, actualCommand, remoteResolution,"
)
assert nested_rejection < remote_resolution < remote_authorize

# Interactive modes still consume arbitrary/unknown input before registry
# dispatch. Validation-only remains handler-owned and suppresses only audit /
# presentation stamping, not lookup or handler execution.
assert execute.index("cliModeDispatchInput(command") < execute.index("if (found)")
assert "const char* result = found->handler(args);" in execute
assert "gCLIValidateOnly" not in execute

# Dynamic registration semantics remain live: appends immediately affect the
# resolver, and pointer identity/lifetime are unchanged.
register = function_body(command_cpp, "void registerCommand(")
assert "commandRegistry[commandRegistrySize] = command;" in register
assert "commandRegistrySize++;" in register
assert "new " not in register
assert "malloc(" not in register


# ---------------------------------------------------------------------------
# Handlers share the debug buffer and static result buffers and copy their
# result only after returning, so they run on one task. executeCommand is
# called only by the executor (cmd_exec, including deferred work it runs) and
# the early-boot path of submitAndExecuteSync; every other caller submits.
# ---------------------------------------------------------------------------
def code_only(source: str) -> str:
    """Blank comments and string/char/raw-string literals, keeping offsets."""

    out = list(source)
    pos, length = 0, len(source)

    def blank(start: int, end: int) -> None:
        for index in range(start, end):
            if out[index] != "\n":
                out[index] = " "

    while pos < length:
        current = source[pos]
        following = source[pos + 1] if pos + 1 < length else ""
        if current == "/" and following == "/":
            end = source.find("\n", pos)
            end = length if end < 0 else end
            blank(pos, end)
            pos = end
        elif current == "/" and following == "*":
            end = source.find("*/", pos + 2)
            end = length if end < 0 else end + 2
            blank(pos, end)
            pos = end
        elif current == '"' and pos > 0 and source[pos - 1] == "R":
            delimiter = source[pos + 1 : source.index("(", pos)]
            end = source.index(")" + delimiter + '"', pos) + len(delimiter) + 2
            blank(pos, end)
            pos = end
        elif current in "\"'":
            if (current == "'" and pos > 0 and source[pos - 1].isdigit()
                    and following.isalnum()):
                pos += 1  # digit separator
                continue
            end = pos + 1
            while end < length and source[end] != current:
                end += 2 if source[end] == "\\" else 1
            blank(pos + 1, end)
            pos = end + 1
        else:
            pos += 1
    return "".join(out)


def body_span(code: str, signature: str) -> tuple:
    """Span of the one definition (not a forward declaration) of signature."""

    definitions = []
    for found in re.finditer(re.escape(signature), code):
        brace = code.find("{", found.end())
        semicolon = code.find(";", found.end())
        if brace >= 0 and (semicolon < 0 or brace < semicolon):
            definitions.append((found.start(), brace))
    assert len(definitions) == 1, f"one definition: {signature!r}"
    start, pos = definitions[0]
    depth = 0
    while True:
        if code[pos] == "{":
            depth += 1
        elif code[pos] == "}":
            depth -= 1
            if depth == 0:
                return start, pos + 1
        pos += 1


sources = {
    path.relative_to(COMPONENT).as_posix(): code_only(path.read_text(encoding="utf-8"))
    for pattern in ("*.cpp", "*.h")
    for path in COMPONENT.rglob(pattern)
    if "test" not in path.relative_to(COMPONENT).parts
}

# (file, enclosing function) for every permitted executeCommand call.
EXECUTE_COMMAND_CALLERS = {
    "HardwareOne.cpp": (
        "static void commandExecTask(",          # the executor
    ),
    "System_Utils.cpp": (
        "bool submitAndExecuteSync(const Command& cmd, String& out,",  # early boot
    ),
    "System_ESPSR.cpp": (
        "static void runVoiceCommandJob(",       # runs on cmd_exec (deferred)
    ),
    # Set 2 (WebPage_ESPNow.cpp) moves this to executeInternalStatusCommand
    # in the after-merge pass; executeCommand can then refuse other callers.
    "WebPage_ESPNow.cpp": ("static void espnowRunInternal(",),
}
call = re.compile(r"(?<![\w.>])executeCommand\s*\(")
for name, code in sorted(sources.items()):
    allowed = [body_span(code, signature)
               for signature in EXECUTE_COMMAND_CALLERS.get(name, ())
               if signature in code]
    for match in call.finditer(code):
        if code[: match.start()].rstrip().endswith("bool"):
            continue  # the declaration / definition
        line = code.count("\n", 0, match.start()) + 1
        assert any(start <= match.start() < end for start, end in allowed), (
            f"{name}:{line} calls executeCommand off the executor; submit it "
            "(submitAndExecuteSync / executeInternalStatusCommand) instead")

# The early-boot call is the gCmdExecQ == nullptr branch only.
utils_code = sources["System_Utils.cpp"]
sync_start, sync_end = body_span(
    utils_code, "bool submitAndExecuteSync(const Command& cmd, String& out,")
sync_code = utils_code[sync_start:sync_end]
early = sync_code.index("if (gCmdExecQ == nullptr)")
early_call = sync_code.index("executeCommand(", early)
assert [m.start() for m in call.finditer(sync_code)] == [early_call]
assert early_call < sync_code.index("ExecReq* r =")
# The voice job is only ever handed to cmd_exec.
espsr_code = sources["System_ESPSR.cpp"]
assert espsr_code.count("runVoiceCommandJob") == 2
assert "submitDeferredToCmdExec(runVoiceCommandJob" in espsr_code
assert "exitHelpAndExecute(" not in sources["HardwareOne.cpp"]

# Web status endpoints submit through the owner helper.
web_code = sources["WebServer_Server.cpp"]
logging_start, logging_end = body_span(
    web_code, "static esp_err_t handleLoggingStatus(")
logging_status = web_code[logging_start:logging_end]
assert "executeInternalStatusCommand(" in logging_status
assert "runInternalStatusCmd" not in web_code
status_start, status_end = body_span(
    utils_code, "bool executeInternalStatusCommand(")
status_helper = utils_code[status_start:status_end]
assert "submitAndExecuteSync(" in status_helper
assert not call.search(status_helper)
assert "SOURCE_INTERNAL" in status_helper
assert "COMMAND_CONTEXT_MODE_INDEPENDENT" in status_helper

# Validate-only state is per task. broadcastOutputCore, the audit
# suppression and the OK stamp read the calling task's CommandContext; the
# global stays a handler mirror written only where the executor runs.
debug_code = sources["System_Debug.cpp"]
core_start, core_end = body_span(debug_code, "static void broadcastOutputCore(")
broadcast_core = debug_code[core_start:core_end]
assert "currentCommandValidateOnly()" in broadcast_core
assert "gCLIValidateOnly" not in broadcast_core
accessor_start, accessor_end = body_span(utils_code, "bool currentCommandValidateOnly()")
accessor = utils_code[accessor_start:accessor_end]
assert "currentCommandContext()" in accessor and "->validateOnly" in accessor
assert "gCLIValidateOnly" not in accessor
for signature in ("static bool commandExecutionLogSuppressed(",
                  "static void stampOkStatus("):
    start, end = body_span(utils_code, signature)
    assert "currentCommandValidateOnly()" in utils_code[start:end], signature
    assert "gCLIValidateOnly" not in utils_code[start:end], signature
writer = re.compile(r"\bgCLIValidateOnly\s*=(?!=)")
VALIDATE_WRITERS = {
    "HardwareOne.cpp": ("static void commandExecTask(",),
    "System_Utils.cpp": ("bool submitAndExecuteSync(const Command& cmd, String& out,",),
}
for name, code in sorted(sources.items()):
    allowed = [body_span(code, signature) for signature in VALIDATE_WRITERS.get(name, ())]
    for match in writer.finditer(code):
        if code[: match.start()].rstrip().endswith("bool"):
            continue  # the definition
        line = code.count("\n", 0, match.start()) + 1
        assert any(start <= match.start() < end for start, end in allowed), (
            f"{name}:{line} writes gCLIValidateOnly outside the executor")

# The shared password helper reports a status and writes its message into the
# caller's buffer; the web handler (still on httpd) reads only that buffer.
user_code = sources["System_User.cpp"]
user_h_code = sources["System_User.h"]
assert "const char* userChangePasswordCore(" not in user_h_code
assert re.search(r"bool userChangePasswordCore\([^;]*char\* message, size_t messageSize\);",
                 user_h_code)
core_start, core_end = body_span(user_code, "bool userChangePasswordCore(")
password_core = user_code[core_start:core_end]
for shared in ("getDebugBuffer", "ensureDebugBuffer", "gDebugBuffer"):
    assert shared not in password_core, shared
page_start, page_end = body_span(web_code, "esp_err_t handlePasswordChangePage(")
password_page = web_code[page_start:page_end]
assert "const bool ok = userChangePasswordCore(current, newp, conf," in password_page
for shared in ("getDebugBuffer", "gDebugBuffer"):
    assert shared not in password_page, shared

print("command resolution source guards passed")
