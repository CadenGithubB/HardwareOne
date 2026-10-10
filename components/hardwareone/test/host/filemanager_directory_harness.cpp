#include <atomic>
#include <cstdint>
#include <cstdio>
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

static std::atomic<uint32_t> gIdentityGeneration{1};
struct PollPauseGuard {};
static uint8_t getPermissions(const String& path, const AuthContext& ctx) {
  check(fsDepth > 0 && ctx.user == auth.user, "fallback permissions retain live caller identity");
  const auto found = permissionMasks.find(path.c_str());
  return found == permissionMasks.end() ? 7 : found->second;
}
namespace VFS {
static bool existsGuarded(const String& path, const AuthContext& ctx) {
  check(fsDepth > 0 && ctx.user == auth.user, "navigation keeps guarded caller identity");
  return nodes.count(path.c_str()) && !denied.count(path.c_str());
}
}

// INSERT_PRODUCTION

static void resetTree(bool mounted) {
  check(fsDepth == 0 && permissionViews == 0, "all traversal lock scopes released");
  nodes.clear(); denied.clear(); events.clear(); permissionMasks.clear();
  sdMounted = mounted; directoryOpens = 0; metadataQueries = 0;
  failAtMetadataQuery = SIZE_MAX; ++gIdentityGeneration;
  nodes["/"] = std::make_shared<Node>(Node{"/", true, 0, {}});
  if (mounted) nodes["/sd"] = std::make_shared<Node>(Node{"/", true, 0, {}});
}
static void add(const std::string& parent, const std::string& name,
                bool folder = false, const std::string& backendName = "") {
  const std::string full = parent + (parent == "/" ? "" : "/") + name;
  auto node = std::make_shared<Node>(Node{backendName.empty() ? full : backendName, folder, folder ? 0u : 12u, {}});
  nodes[full] = node;
  nodes.at(parent)->children.push_back(node);
}
static std::string numbered(unsigned i, const char* extension = ".txt") {
  char name[40]; std::snprintf(name, sizeof(name), "file-%03u%s", i, extension); return name;
}
static bool binsOnly(const FileEntry& item) { return String(item.name).endsWith(".bin"); }
static void expectItems(FileManager& manager, const std::vector<std::string>& expected) {
  check(manager.getItemCount() == static_cast<int>(expected.size()), "visible count differs from fixture entries");
  FileEntry entry{};
  check(!manager.getItem(-1, entry) && !manager.getItem(static_cast<int>(expected.size()), entry), "invalid index rejected");
  for (size_t i = 0; i < expected.size(); ++i) {
    if (!manager.getItem(static_cast<int>(i), entry)) {
      throw std::runtime_error("reported index " + std::to_string(i) + " is missing; expected " + expected[i]);
    }
    if (entry.name != expected[i]) {
      throw std::runtime_error("index " + std::to_string(i) + " returned " + entry.name + "; expected " + expected[i]);
    }
  }
}
static void virtualBoundary(unsigned count, bool mounted) {
  resetTree(mounted);
  std::vector<std::string> expected;
  if (mounted) expected.push_back("sd");
  for (unsigned i = 0; i < count; ++i) { add("/", numbered(i)); expected.push_back(numbered(i)); }
  FileManager manager;
  check(manager.navigate("/"), "root navigation succeeds");
  expectItems(manager, expected);
}
static void pickerBeyondCache(const char* parent) {
  resetTree(true);
  if (std::strcmp(parent, "/sd/deep") == 0) add("/sd", "deep", true, "/deep");
  const std::string prefix = VFS::stripSdPrefix(parent).c_str();
  for (unsigned i = 0; i < 257; ++i) {
    add(parent, numbered(i), false, prefix + (prefix == "/" ? "" : "/") + numbered(i));
  }
  add(parent, "wanted.bin", false, prefix + (prefix == "/" ? "" : "/") + "wanted.bin");
  FileManager manager;
  check(manager.navigate(parent), "picker directory navigation succeeds");
  manager.setVisibilityFilter(binsOnly);
  std::vector<std::string> expected;
  if (std::strcmp(parent, "/") == 0) expected.push_back("sd");
  expected.push_back("wanted.bin");
  expectItems(manager, expected);
}
static void filteredFallback(const char* parent) {
  resetTree(true);
  if (std::strcmp(parent, "/sd/deep") == 0) add("/sd", "deep", true, "/deep");
  const std::string prefix = VFS::stripSdPrefix(parent).c_str();
  std::vector<std::string> expected;
  if (std::strcmp(parent, "/") == 0) expected.push_back("sd");
  for (unsigned i = 0; i < 520; ++i) {
    const std::string name = numbered(i, i % 2 ? ".txt" : ".bin");
    add(parent, name, false, prefix + (prefix == "/" ? "" : "/") + name);
    if (i % 2 == 0) expected.push_back(name);
  }
  FileManager manager;
  check(manager.navigate(parent), "large picker directory navigation succeeds");
  manager.setVisibilityFilter(binsOnly);
  expectItems(manager, expected);
}
static void compatibility() {
  resetTree(true);
  add("/sd", "deep", true, "/deep");
  add("/sd/deep", ".hidden.bin", false, "/deep/.hidden.bin");
  add("/sd/deep", "nested/child.bin", false, "/deep/nested/child.bin");
  add("/sd/deep", "bare.bin", false, "bare.bin");
  add("/sd/deep", "full.bin", false, "/deep/full.bin");
  add("/sd/deep", "skip.txt", false, "/deep/skip.txt");
  add("/sd/deep", "folder", true, "/deep/folder");
  for (unsigned i = 0; i < 120; ++i) add("/sd/deep/folder", numbered(i));
  FileManager manager;
  check(manager.navigate("/sd/deep"), "SD subfolder is navigable");
  expectItems(manager, {"bare.bin", "full.bin", "skip.txt", "folder"});
  const size_t opens = directoryOpens;
  check(manager.refresh() && directoryOpens == opens, "same identity refresh uses existing cache");
  manager.setVisibilityFilter(binsOnly);
  expectItems(manager, {"bare.bin", "full.bin", "folder"});
  manager.setCountFolderChildren(true);
  FileEntry item{};
  check(manager.getItem(2, item) && item.isFolder && item.childCount == 99, "opt-in folder count retains cap");
  manager.setVisibilityFilter(nullptr);
  expectItems(manager, {"bare.bin", "full.bin", "skip.txt", "folder"});
  add("/sd/deep", "new.bin", false, "/deep/new.bin");
  ++gIdentityGeneration;
  check(manager.refresh(), "identity change reloads directory");
  expectItems(manager, {"bare.bin", "full.bin", "skip.txt", "folder", "new.bin"});
  check(fsDepth == 0 && permissionViews == 0, "successful paths release lock and metadata view");
}
static void fullPermissionPaths() {
  resetTree(false);
  const std::string first(75, 'a'), last(75, 'z');
  add("/", first);
  for (unsigned i = 1; i < 256; ++i) add("/", numbered(i));
  add("/", last);
  permissionMasks["/" + first] = 9;
  permissionMasks["/" + last] = 17;
  permissionMasks["/" + first.substr(0, 63)] = 0;
  permissionMasks["/" + last.substr(0, 63)] = 0;
  FileManager manager;
  check(manager.navigate("/"), "long-name fixture loads");
  FileEntry item{};
  check(manager.getItem(0, item) && item.name == first.substr(0, 63) && item.permissions == 9,
        "cached display name stays bounded while permissions use the complete native path");
  check(manager.getItem(256, item) && item.name == last.substr(0, 63) && item.permissions == 17,
        "fallback display name stays bounded while permissions use the complete native path");
}
static void deniedDirectory() {
  resetTree(false);
  add("/", "visible.txt");
  denied.insert("/");
  FileManager manager;
  check(!manager.navigate("/") && !manager.refresh(), "denied directory fails both navigation and scan");
  check(fsDepth == 0 && permissionViews == 0, "denied paths release locks without a role view");
  denied.clear();
  check(manager.refresh(), "initial denial remains retryable without an identity-generation bump");
  expectItems(manager, {"visible.txt"});
  check(!manager.navigate("/visible.txt"), "regular file cannot become the current directory");
  check(std::strcmp(manager.getCurrentPath(), "/") == 0 && fsDepth == 0, "failed navigation keeps prior path and releases lock");
}
#if TEST_DIRECTORY_READER
static void readerContract() {
  resetTree(true);
  add("/", "empty"); nodes.at("/empty")->name = "";
  add("/", "nested/child.bin");
  add("/", "rooted.txt");
  add("/", "leaf.txt", false, "leaf.txt");
  FsLockGuard guard("reader.contract");
  File directory = VFS::openGuarded("/", "r", auth);
  VFS::DirectoryEntry entry;
  {
    VFS::DirectoryReader reader(directory, "/");
    check(reader.next(entry) && entry.name == "sd" && entry.path == "/sd" && entry.isVirtual && entry.isFolder,
          "virtual mount comes first with its logical path");
    check(reader.next(entry) && entry.name == "rooted.txt" && entry.path == "/rooted.txt" &&
          !entry.isVirtual && !entry.isFolder && entry.size == 12, "empty and nested entries skipped before rooted file");
    nodes.at("/rooted.txt")->name = "changed-after-child-close";
    check(entry.name == "rooted.txt", "entry owns its name after child close/backend mutation");
    check(reader.next(entry) && entry.name == "leaf.txt" && entry.path == "/leaf.txt", "native leaf names accepted");
    const VFS::DirectoryEntry previous = entry;
    check(!reader.next(entry) && !reader.next(entry), "end of directory remains exhausted");
    check(entry.name == previous.name && entry.path == previous.path && entry.size == previous.size &&
          entry.isFolder == previous.isFolder && entry.isVirtual == previous.isVirtual, "false leaves caller's prior output unchanged");
  }
  check(static_cast<bool>(directory), "reader destruction does not close borrowed parent");
  directory.close();
  check(!directory, "parent lifetime remains caller-owned");
}
#endif
int main(int argc, char** argv) {
  const std::string selected = argc > 1 ? argv[1] : "all";
  unsigned failures = 0;
  const auto run = [&](const std::string& label, const std::function<void()>& test) {
    try { test(); std::printf("PASS %s\n", label.c_str()); }
    catch (const std::exception& error) { ++failures; std::printf("FAIL %s: %s\n", label.c_str(), error.what()); }
  };
  if (selected == "all" || selected == "virtual-boundary") {
    for (bool mounted : {false, true}) for (unsigned count : {255u, 256u, 257u}) {
      run("virtual-boundary mounted=" + std::to_string(mounted) + " real=" + std::to_string(count),
          [=]() { virtualBoundary(count, mounted); });
    }
  }
  if (selected == "all" || selected == "picker-beyond-cache") {
    for (const char* parent : {"/", "/sd/deep"}) {
      run(std::string("picker-beyond-cache ") + parent, [=]() { pickerBeyondCache(parent); });
      run(std::string("filtered-fallback ") + parent, [=]() { filteredFallback(parent); });
    }
  }
  if (selected == "all" || selected == "compatibility") {
    run("hidden/picker/SD/identity/count compatibility", compatibility);
    run("complete permission paths with bounded display names", fullPermissionPaths);
    run("denied-directory cleanup and retry", deniedDirectory);
#if TEST_DIRECTORY_READER
    run("shared reader entry/output/lifetime contract", readerContract);
#endif
  }
  std::printf("FileManager production traversal: %zu assertions, %u failed scenarios\n", checks, failures);
  return failures ? 1 : 0;
}
