#!/usr/bin/env python3
"""Source contract for the operation-scoped listing permission view.

The firmware implementation is coupled to Arduino File, FreeRTOS mutex
ownership, and the live users roster. These checks deliberately inspect the
production sources rather than maintaining a permissive host-side imitation of
that security boundary.
"""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
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
assert ESPNOW_FS_LIST_CPP.count("e.perms = getPermissions(") == 2

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
assert "permissionsForResolvedRole(normalizedPath, scope_, role)" in query
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
assert listing.count("listingPermissions->forPath") == 3
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
assert load_dir.count("listingPermissions.forPath") == 2
assert ".permissions = getPermissions(" not in load_dir

get_item = function_body(FILE_MANAGER_CPP, "bool FileManager::getItem")
assert "entry.permissions = getPermissions(fullPath, ctx)" in get_item
assert "LockedListingPermissions" not in get_item

print("filesystem listing permission contract: OK")
