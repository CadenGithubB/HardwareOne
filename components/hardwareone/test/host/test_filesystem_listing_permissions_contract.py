#!/usr/bin/env python3
"""Source contract for the operation-scoped listing permission view.

The firmware implementation is coupled to Arduino File, FreeRTOS mutex
ownership, and the live users roster. These checks deliberately inspect the
production sources rather than maintaining a permissive host-side imitation of
that security boundary.

The file-permission owner check at the end compiles the real rule table and
canX code (filesystem_role_matrix_harness.cpp) with the requiresAdmin column
of every file command row, and asserts one per-role read/write/create/delete
matrix through both the command table and the direct web file endpoints.
Options: --cxx <compiler> (default c++), --sanitize (ASan + UBSan).
"""

import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
WEB_SERVER_CPP = (ROOT / "WebServer_Server.cpp").read_text()
WEB_UTILS_H = (ROOT / "WebServer_Utils.h").read_text()
FS_CPP = (ROOT / "System_Filesystem.cpp").read_text()
FS_H = (ROOT / "System_Filesystem.h").read_text()
FS_INTERNAL_H = (ROOT / "System_Filesystem_Internal.h").read_text()
FILE_MANAGER_CPP = (ROOT / "System_FileManager.cpp").read_text()
USER_CPP = (ROOT / "System_User.cpp").read_text()
VFS_CPP = (ROOT / "System_VFS.cpp").read_text()
VFS_H = (ROOT / "System_VFS.h").read_text()
ESPNOW_FS_LIST_CPP = (ROOT / "System_ESPNow_FsList.cpp").read_text()


def function_body(source: str, signature: str) -> str:
    start = source.find(signature)
    assert start >= 0, f"missing function: {signature}"
    brace = source.find("{", start)
    assert brace >= 0, f"missing body: {signature}"
    depth = 0
    for pos in range(brace, len(source)):
        if source[pos] == "{":
            depth += 1
        elif source[pos] == "}":
            depth -= 1
            if depth == 0:
                return source[brace : pos + 1]
    raise AssertionError(f"unterminated body: {signature}")


# Internal means internal: no resolved-role object or token in the established
# public API, and no way for VFS enforcement to accept the listing view.
assert "LockedListingPermissions" not in FS_H
assert "LockedListingPermissions" not in VFS_H
assert "System_Filesystem_Internal.h" not in VFS_CPP
assert FS_H.count(
    "uint8_t getPermissions(const String& path, const AuthContext& ctx);"
) == 1
assert FS_H.count(
    "uint8_t getDirPerms(const String& dirPath, const AuthContext& ctx);"
) == 1

# Keep the internal view out of every other production translation unit. In
# particular, it must not become an enforcement shortcut in VFS or migrate the
# SYSTEM ESP-NOW listing path, whose live one-shot behavior is intentional.
for source_path in ROOT.glob("*.cpp"):
    if source_path.name in {"System_Filesystem.cpp", "System_FileManager.cpp"}:
        continue
    source = source_path.read_text()
    assert "LockedListingPermissions" not in source, source_path.name
    assert "System_Filesystem_Internal.h" not in source, source_path.name
assert "LockedListingPermissions" not in ESPNOW_FS_LIST_CPP
assert ESPNOW_FS_LIST_CPP.count("e.perms = getPermissions(") == 1

class_pos = FS_INTERNAL_H.index("class LockedListingPermissions final")
class_text = FS_INTERNAL_H[class_pos:]
assert "LockedListingPermissions(const LockedListingPermissions&) = delete" in class_text
assert "LockedListingPermissions(LockedListingPermissions&&) = delete" in class_text
assert class_text.index("FsLockGuard lock_") < class_text.index("String scope_")
assert "TaskHandle_t ownerTask_" in class_text
assert "generation" not in class_text.lower()

# All established synthetic identities keep flowing through the one canonical
# resolver. Named accounts use the combined read exactly once, while the two
# deliberately different parsers remain separate.
resolve_role = function_body(
    FS_CPP, "static FsRole resolveRole(const AuthContext& ctx)"
)
assert 'ctx.transport == SOURCE_INTERNAL && ctx.user == "system"' in resolve_role
assert 'ctx.user == "AuthBypass"' in resolve_role
assert 'ctx.transport == SOURCE_INTERNAL && ctx.user == "uart-session"' in resolve_role
assert 'ctx.path == "/g2evenai"' in resolve_role
assert "ctx.transport == SOURCE_ESPNOW && isSuperAdminUser(ctx.user)" in resolve_role
assert resolve_role.count("getUserRoleAndSuper(ctx.user, storedRole, isSuper)") == 1

role_and_super = function_body(
    USER_CPP,
    "bool getUserRoleAndSuper(const String& who, String& roleOut, bool& isSuperOut)",
)
assert role_and_super.count("readRosterJson(") == 1
assert "roleFromRosterJson(json, who, roleOut)" in role_and_super
assert "superFromRosterJson(json, who)" in role_and_super

role_parser = function_body(
    USER_CPP,
    "static bool roleFromRosterJson(const String& json, const String& username,\n"
    "                               String& roleOut) {",
)
assert "deserializeJson(doc, json)" in role_parser
assert 'user["banned"] | false' in role_parser
assert "role.toLowerCase()" in role_parser
assert "isKnownUserRole(role)" in role_parser

super_parser = function_body(
    USER_CPP,
    "static bool superFromRosterJson(const String& json, const String& who) {",
)
assert "deserializeJson" not in super_parser
assert 'json.indexOf("\\\"username\\\""' in super_parser
assert 'json.indexOf("\\\"role\\\""' in super_parser
assert 'role == "superadmin"' in super_parser
assert "anyExplicitSuper" in super_parser

# Construction owns/reuses the filesystem lock before resolving a named role.
# The live bond tuple is marked dynamic regardless of current token state.
ctor = function_body(
    FS_CPP,
    "LockedListingPermissions::LockedListingPermissions(const AuthContext& ctx)",
)
assert 'lock_("filesystem.listingPermissions")' in FS_CPP
assert "isFsLockedByCurrentTask()" in ctor
assert "ctx.transport == SOURCE_ESPNOW && ctx.user == kBondAdminUser" in ctor
assert "dynamicBond_ = true" in ctor
assert ctor.index("dynamicBond_ = true") < ctor.index("resolveRole(ctx)")
assert ctor.count("resolveRole(ctx)") == 1
assert "getUserAuthorizationRole" not in ctor
assert "isAdminUser" not in ctor
assert "logFsAccessDeny" not in ctor

ready = function_body(FS_CPP, "bool LockedListingPermissions::ready() const")
assert "ownerTask_ == xTaskGetCurrentTaskHandle()" in ready
assert "isFsLockedByCurrentTask()" in ready

query = function_body(
    FS_CPP, "uint8_t LockedListingPermissions::forPath(const String& path) const"
)
assert "if (!ready()) return 0" in query
assert "normalizeFsPath(path, normalizedPath)" in query
assert "isSuperAdminUser(kBondAdminUser)" in query
assert "permissionsForResolvedRole(normalizedPath, scope_, role, transcriptAccountId_)" in query
assert "transcriptAccountResolved_" in query
assert "transcriptAccountId(normalizedPath, user_, role)" in query
assert "logFsAccessDeny" not in query
assert "DEBUG_" not in query

child_query = function_body(
    FS_CPP,
    "uint8_t LockedListingPermissions::forChildOf(const String& dirPath) const",
)
assert "if (!ready()) return 0" in child_query
assert "return forPath(testPath)" in child_query

# Both one-shot and batched aggregate queries share normalization, scope, and
# the special sensitive/image masks. This catches the historical scope drift.
resolved = function_body(FS_CPP, "static uint8_t permissionsForResolvedRole")
assert "!pathWithinScope(normalizedPath, scope)" in resolved
assert "hasSensitiveExtension(normalizedPath)" in resolved
assert "transcriptPermissionMask(normalizedPath, accountId, role)" in resolved
assert "isImageFile(normalizedPath)" in resolved
assert "logFsAccessDeny" not in resolved
assert "DEBUG_" not in resolved

one_shot = function_body(
    FS_CPP, "uint8_t getPermissions(const String& path, const AuthContext& ctx)"
)
assert "normalizeFsPath(path, normalizedPath)" in one_shot
assert "permissionsForResolvedRole(normalizedPath, ctx.scope" in one_shot

# Only the repeated listing scans migrate. buildFilesListJson shares the view
# with dirPerms; FileManager's rare one-item fallback deliberately remains on
# the ordinary live API.
listing = function_body(FS_CPP, "static bool buildFilesListingImpl")
assert "std::optional<FsInternal::LockedListingPermissions>" in listing
assert "if (asJson && listingPermissions == nullptr)" in listing
assert listing.count("listingPermissions->forPath") == 1
assert "getPermissions(" not in listing

public_listing = function_body(
    FS_CPP,
    "bool buildFilesListing(const String& inPath, String& out, bool asJson,",
)
assert "buildFilesListingImpl" in public_listing
assert "nullptr" in public_listing

listing_json = function_body(FS_CPP, "bool buildFilesListJson")
assert "LockedListingPermissions listingPermissions(ctx)" in listing_json
assert "buildFilesListingImpl" in listing_json
assert "listingPermissions.forChildOf(path)" in listing_json

load_dir = function_body(FILE_MANAGER_CPP, "bool FileManager::loadDirectory()")
assert "LockedListingPermissions listingPermissions(ctx)" in load_dir
assert load_dir.count("listingPermissions.forPath") == 1
assert ".permissions = getPermissions(" not in load_dir

get_item = function_body(FILE_MANAGER_CPP, "bool FileManager::getItem")
assert "entry.permissions = getPermissions(item.path, ctx)" in get_item
assert "LockedListingPermissions" not in get_item

# ---------------------------------------------------------------------------
# One owner for file permissions: the path rule table decides for every
# transport. The command rows carry no admin flag of their own, and the web
# endpoints add only the table-derived isAdminOnlyPath pre-check.
# ---------------------------------------------------------------------------
FILE_COMMANDS = ("files", "mkdir", "rmdir", "filecreate", "fileview", "fileread",
                 "filewrite", "filedelete", "filerename")


def file_command_gates(source: str) -> dict:
    start = source.index("const CommandEntry filesystemCommands[] = {")
    end = source.index("const size_t filesystemCommandsCount", start)
    rows = re.findall(r'\{\s*"(\w+)",\s*"(?:[^"\\]|\\.)*",\s*(true|false),\s*(cmd_\w+)',
                      source[start:end])
    gates = {name: flag == "true" for name, flag, handler in rows
             if handler == "cmd_" + name}
    missing = [name for name in FILE_COMMANDS if name not in gates]
    assert not missing, f"file command rows not found: {missing}"
    return {name: gates[name] for name in FILE_COMMANDS}


def web_handlers_gate_like_the_model() -> None:
    pre_check = re.compile(r"if \(isAdminOnlyPath\((\w+)\) && !isAdminUser\(ctx\.user\)\)")
    for handler in ("esp_err_t handleFileRead(", "esp_err_t handleFileWrite(",
                    "esp_err_t handleFileUpload(", "esp_err_t handleFilesList(",
                    "esp_err_t handleFilesCreate(", "esp_err_t handleFileView("):
        body = function_body(WEB_SERVER_CPP, handler)
        assert pre_check.search(body), f"{handler} lost the table-derived admin-path pre-check"
        # No second, role-based refusal outside the table.
        assert "currentExecIsAdmin" not in body, handler
    create = function_body(WEB_SERVER_CPP, "esp_err_t handleFilesCreate(")
    assert "executeUnifiedWebCommand(" in create
    assert '"mkdir "' in create and "filecreate " in create
    # The Files page sends the remaining verbs through the command executor.
    for verb in ("filedelete ", "filerename ", "rmdir "):
        assert verb in WEB_UTILS_H, verb


def run_role_matrix(cxx: str, sanitize: bool) -> None:
    here = ROOT / "test" / "host"
    sys.path.insert(0, str(here))
    from test_web_batch_handlers import extract_block

    header = FS_H
    internal = re.sub(r"^#include.*\n", "", FS_INTERNAL_H, flags=re.M)
    pieces = [extract_block(header, "enum FilePermission") + ";",
              extract_block(FS_CPP, "struct PathRule") + ";",
              extract_block(FS_CPP, "static const PathRule sPathRules[]") + ";"]
    for marker in ("static const PathRule& lookupRule(", "static bool hasSensitiveExtension(",
                   "static bool isImageFile(", "bool normalizeFsPath(", "enum class FsRole"):
        pieces.append(extract_block(FS_CPP, marker) + (";" if marker.startswith("enum") else ""))
    for marker in ("static inline bool isUnrestrictedRole(", "static FsRole resolveRole(",
                   "static uint8_t permsForRole(", "static bool pathWithinScope(",
                   "static uint32_t transcriptAccountId(", "static uint8_t transcriptPermissionMask(",
                   "static uint8_t permissionsForResolvedRole("):
        pieces.append(extract_block(FS_CPP, marker))
    for marker in ("static bool checkPerm(", "bool canRead ", "bool canEdit ", "bool canDelete ",
                   "bool canRename ", "bool canCreate ", "bool canImport ",
                   "bool isAdminOnlyPath(", "uint8_t getPermissions("):
        pieces.append(extract_block(FS_CPP, marker))
    gates = file_command_gates(FS_CPP)
    gate_rows = "\n".join(f'    {{"{name}", {"true" if flag else "false"}}},'
                          for name, flag in gates.items())
    harness = (here / "filesystem_role_matrix_harness.cpp").read_text()
    policy = (ROOT / "Transcript_PathPolicy.h").read_text().replace("#pragma once", "")
    for marker, value in (("// INSERT_POLICY", policy), ("// INSERT_INTERNAL", internal),
                          ("// INSERT_PRODUCTION", "\n\n".join(pieces)),
                          ("// INSERT_COMMAND_GATES", gate_rows)):
        assert harness.count(marker) == 1, marker
        harness = harness.replace(marker, value)
    with tempfile.TemporaryDirectory(prefix="hw1-fs-role-matrix-") as temp:
        unit, exe = Path(temp) / "matrix.cpp", Path(temp) / "matrix"
        unit.write_text(harness)
        cmd = [cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic", str(unit), "-o", str(exe)]
        if sanitize:
            cmd[1:1] = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        subprocess.run(cmd, check=True, timeout=180)
        result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=60)
        sys.stdout.write(result.stdout)
        sys.stderr.write(result.stderr)
        assert result.returncode == 0, "file permission matrix disagrees between transports"


parser = argparse.ArgumentParser()
parser.add_argument("--cxx", default="c++")
parser.add_argument("--sanitize", action="store_true")
ARGS = parser.parse_args()

web_handlers_gate_like_the_model()
run_role_matrix(ARGS.cxx, ARGS.sanitize)
admin_only = [name for name, flag in file_command_gates(FS_CPP).items() if flag]
assert not admin_only, f"file commands still admin-only beside the path rules: {admin_only}"

print("filesystem listing permission contract: OK")
