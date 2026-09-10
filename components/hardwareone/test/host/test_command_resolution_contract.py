#!/usr/bin/env python3
"""Source-contract guards for single-resolution command execution."""

from pathlib import Path


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
assert "commandRequiresAdmin(" not in authorize
assert "commandRequiresSuperAdmin(" not in authorize
assert "redactCmdForAudit(" not in authorize

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

print("command resolution source guards passed")
