#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

static bool gFailPsram = false, gFailInternal = false;
// INSERT_BOUNDARIES

static constexpr unsigned ESPNOW_V4_MAX_PLAINTEXT = 202;
static constexpr uint8_t ESPNOW_V4_TYPE_FS_LIST_REPLY = 123;
// INSERT_PRODUCTION_TYPES

#define ERROR_MEMORYF(...) debugStub(__VA_ARGS__)
#define DEBUG_ESPNOWF(...) debugStub(__VA_ARGS__)
#define WARN_ESPNOWF(...) debugStub(__VA_ARGS__)
static bool allocationFails = false, pathAllowed = true, existenceDenied = false;
static unsigned identityDepth = 0, allocations = 0, releases = 0;
enum class AllocPref { PreferPSRAM };
static void* ps_alloc(size_t size, AllocPref, const char*) {
  if (allocationFails) return nullptr;
  void* result = std::malloc(size);
  check(result != nullptr, "host reply allocation succeeds");
  ++allocations;
  return result;
}
static void releaseReply(void* allocation) {
  check(fsDepth == 0 && identityDepth == 0, "reply buffer released after lock and identity scope");
  ++releases;
  std::free(allocation);
}
// The handler installs the identity the requester gate returns: the bond
// identity for the bonded peer. The SYSTEM_IDENTITY_SCOPE alias keeps older
// handler snapshots compiling.
struct ExecIdentityGuard {
  String previous;
  explicit ExecIdentityGuard(const AuthContext& install) : previous(auth.user) {
    ++identityDepth; auth.user = install.user;
  }
  ~ExecIdentityGuard() { auth.user = previous; --identityDepth; }
};
static AuthContext systemIdentity(const char*) { AuthContext c; c.user = "system"; return c; }
#define SYSTEM_IDENTITY_SCOPE(reason) ExecIdentityGuard identityScope(systemIdentity(reason))
static bool fsRpcPathAllowed(const char*) { return pathAllowed; }
static bool fsRpcRequesterIdentity(const uint8_t*, AuthContext& out) { out.user = "bond-admin"; return true; }
static uint8_t getPermissions(const String& path, const AuthContext& ctx) {
  check(fsDepth > 0 && identityDepth == 1 && ctx.user == "bond-admin",
        "wire permission lookup remains inside FS lock with the established bond identity");
  events.push_back("perms:" + std::string(path.c_str()));
  const auto found = permissionMasks.find(path.c_str());
  return found == permissionMasks.end() ? 7 : found->second;
}
namespace VFS {
static bool existsGuarded(const String& path, const AuthContext& ctx) {
  check(identityDepth == 1 && ctx.user == "bond-admin", "existence query retains the guarded bond identity");
  return !existenceDenied && nodes.count(path.c_str());
}
}
static uint32_t generateMessageId() { return 42; }
static std::vector<uint8_t> sentPayload;
static unsigned sends = 0;
namespace espnowtx {
static bool sendAeadSync(const uint8_t*, uint8_t type, uint8_t, uint32_t,
                         const uint8_t* payload, uint16_t size, unsigned, unsigned) {
  check(fsDepth == 0 && identityDepth == 0, "network send occurs after directory lock and identity release");
  check(type == ESPNOW_V4_TYPE_FS_LIST_REPLY, "response has the listing wire type");
  sentPayload.assign(payload, payload + size);
  ++sends;
  return true;
}
}
#define free releaseReply
// INSERT_PRODUCTION_HANDLER
#undef free

static void resetTree(bool mounted = true) {
  check(fsDepth == 0 && identityDepth == 0 && allocations == releases, "previous request cleaned up");
  nodes.clear(); denied.clear(); events.clear(); permissionMasks.clear(); sentPayload.clear();
  sdMounted = mounted; filesystemReady = true; pathAllowed = true;
  allocationFails = existenceDenied = false;
  allocations = releases = sends = 0; directoryOpens = 0;
  nodes["/"] = std::make_shared<Node>(Node{"/", true, 0, {}});
}
static void add(const std::string& parent, const std::string& name,
                bool folder = false, const std::string& backendName = "") {
  const std::string full = parent + (parent == "/" ? "" : "/") + name;
  auto node = std::make_shared<Node>(Node{backendName.empty() ? full : backendName, folder, 12, {}});
  nodes[full] = node;
  nodes.at(parent)->children.push_back(node);
}
static std::string numbered(unsigned i) {
  char name[24]; std::snprintf(name, sizeof(name), "file-%03u.txt", i); return name;
}
struct Reply {
  V4PayloadFsListReplyHeader header{};
  std::vector<V4PayloadFsEntry> entries;
};
static Reply request(const char* path, uint16_t start = 0, uint16_t maximum = 32) {
  V4PayloadFsListReq req{};
  req.reqId = 99; req.startIndex = start; req.maxEntries = maximum;
  strlcpy(req.path, path, sizeof(req.path));
  const uint8_t mac[6]{};
  sends = 0;
  processListDeferred(mac, req);
  check(sends == 1 && fsDepth == 0 && identityDepth == 0 && allocations == releases,
        "request sends exactly one reply and releases resources");
  check(sentPayload.size() >= sizeof(V4PayloadFsListReplyHeader), "reply contains a full header");
  Reply result;
  std::memcpy(&result.header, sentPayload.data(), sizeof(result.header));
  check(result.header.reqId == req.reqId && std::string(result.header.path) == path,
        "reply echoes request correlation and path");
  check(result.header.reserved == 0, "header reserved bytes stay zero");
  check(sentPayload.size() == sizeof(result.header) + result.header.entryCount * sizeof(V4PayloadFsEntry),
        "payload contains exactly the reported entries");
  result.entries.resize(result.header.entryCount);
  if (!result.entries.empty())
    std::memcpy(result.entries.data(), sentPayload.data() + sizeof(result.header),
                result.entries.size() * sizeof(V4PayloadFsEntry));
  for (const auto& entry : result.entries)
    for (uint8_t reserved : entry.reserved) check(reserved == 0, "entry reserved bytes stay zero");
  return result;
}
static void testPagination() {
  resetTree();
  add("/", ".hidden");
  for (unsigned i = 0; i < 40; ++i) add("/", numbered(i));
  for (uint16_t start : {0, 31, 32, 41, 50}) {
    const Reply reply = request("/", start, 100);
    const unsigned expected = start < 41 ? std::min(32u, 41u - start) : 0;
    check(reply.header.status == FS_LIST_STATUS_OK && reply.header.totalEntries == 41 &&
          reply.header.entryCount == expected && reply.header.nextStartIndex == start + expected &&
          reply.header.hasMore == (start + expected < 41), "paging total/cap/next/hasMore remain consistent");
    for (unsigned i = 0; i < expected; ++i) {
      const unsigned index = start + i;
      check(reply.entries[i].name == (index == 0 ? std::string("sd") : numbered(index - 1)),
            "virtual mount comes first, dotfiles do not occupy a page index");
    }
  }
  Reply limited = request("/", 31, 1);
  check(limited.entries.size() == 1 && limited.entries[0].name == numbered(30) && limited.header.hasMore,
        "requested cap below 32 is respected");
  limited = request("/", 0, 0);
  check(limited.entries.empty() && limited.header.totalEntries == 41 && limited.header.hasMore &&
        limited.header.nextStartIndex == 0, "zero-cap behavior remains unchanged");
  sdMounted = false;
  const Reply noSd = request("/");
  check(noSd.header.totalEntries == 40 && noSd.entries.front().name == numbered(0),
        "unmounted SD does not shift directory indexes");
}
static void testNestedNamesAndPermissions() {
  resetTree(false);
  add("/", "sd", true); add("/sd", "deep", true, "/deep");
  add("/sd/deep", "first.txt", false, "/deep/first.txt");
  add("/sd/deep", "second.txt", false, "second.txt");
  add("/sd/deep", "folder", true, "/deep/folder");
  add("/sd/deep", ".hidden", false, "/deep/.hidden");
  add("/sd/deep", "invalid/nested.txt", false, "/deep/invalid/nested.txt");
  const std::string longName(90, 'x');
  add("/sd/deep", longName, false, "/deep/" + longName);
  permissionMasks["/sd/deep/" + longName] = 3;
  permissionMasks["/sd/deep/" + longName.substr(0, 63)] = 0;
  const Reply reply = request("/sd/deep");
  check(reply.header.totalEntries == 4 && reply.entries.size() == 4 && !reply.header.hasMore,
        "nested SD includes direct children only and excludes dotfiles");
  check(std::string(reply.entries[0].name) == "first.txt" && std::string(reply.entries[1].name) == "second.txt",
        "absolute backend paths and basename-only names both become leaves");
  check(reply.entries[0].size == 12 && !reply.entries[0].isFolder &&
        reply.entries[2].size == 0 && reply.entries[2].isFolder, "file/folder metadata retains wire semantics");
  check(reply.entries[3].name == longName.substr(0, 63) && reply.entries[3].perms == 3,
        "wire truncation never changes the path used for permissions");
}
static void testErrors() {
  const auto expect = [](uint8_t status) {
    const Reply reply = request("/");
    check(reply.header.status == status && reply.entries.empty() && !reply.header.hasMore &&
          reply.header.totalEntries == 0, "error reply has expected status and no partial listing");
  };
  resetTree(); allocationFails = true; expect(FS_LIST_STATUS_NOT_READY);
  check(directoryOpens == 0 && allocations == 0, "allocation failure performs no directory IO");
  resetTree(); filesystemReady = false; expect(FS_LIST_STATUS_NOT_READY);
  resetTree(); pathAllowed = false; expect(FS_LIST_STATUS_PERM_DENIED);
  check(directoryOpens == 0, "path-policy rejection happens before guarded open");
  resetTree(); existenceDenied = true; expect(FS_LIST_STATUS_NOT_FOUND);
  resetTree(); denied.insert("/"); expect(FS_LIST_STATUS_PERM_DENIED);
  resetTree(); nodes.at("/")->directory = false; expect(FS_LIST_STATUS_NOT_A_DIR);
  resetTree(false);
  const Reply empty = request("/");
  check(empty.header.status == FS_LIST_STATUS_OK && empty.entries.empty() && !empty.header.hasMore &&
        empty.header.totalEntries == 0, "empty directory is a successful empty page");
}
int main() {
  testPagination(); testNestedNamesAndPermissions(); testErrors();
  std::printf("ESP-NOW directory handler: %zu checks passed\n", checks);
}
