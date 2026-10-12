#include <algorithm>
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
// Mutation doubles keep the fixture tree consistent: a node leaves its parent's
// listing when removed or renamed, so a later directory walk reflects it.
static std::vector<std::string> operations;
static std::string parentOf(const std::string& path) {
  const size_t slash = path.rfind('/');
  return slash == 0 ? "/" : path.substr(0, slash);
}
static std::shared_ptr<Node> detach(const std::string& path) {
  const auto it = nodes.find(path);
  if (it == nodes.end() || !nodes.count(parentOf(path))) return nullptr;
  auto node = it->second;
  nodes.erase(it);
  auto& siblings = nodes.at(parentOf(path))->children;
  siblings.erase(std::remove(siblings.begin(), siblings.end(), node), siblings.end());
  return node;
}
namespace VFS {
static bool existsGuarded(const String& path, const AuthContext& ctx) {
  check(fsDepth > 0 && ctx.user == auth.user, "navigation keeps guarded caller identity");
  return nodes.count(path.c_str()) && !denied.count(path.c_str());
}
static bool removeGuarded(const String& path, const AuthContext& ctx) {
  check(fsDepth > 0 && ctx.user == auth.user, "removal keeps guarded caller identity");
  operations.push_back("remove:" + std::string(path.c_str()));
  if (denied.count(path.c_str())) return false;
  const auto node = nodes.count(path.c_str()) && !nodes.at(path.c_str())->directory ? detach(path.c_str()) : nullptr;
  return node != nullptr;
}
static bool rmdirGuarded(const String& path, const AuthContext& ctx) {
  check(fsDepth > 0 && ctx.user == auth.user, "directory removal keeps guarded caller identity");
  operations.push_back("rmdir:" + std::string(path.c_str()));
  if (denied.count(path.c_str())) return false;
  const auto it = nodes.find(path.c_str());
  if (it == nodes.end() || !it->second->directory || !it->second->children.empty()) return false;
  return detach(path.c_str()) != nullptr;
}
static bool renameGuarded(const String& from, const String& to, const AuthContext& ctx) {
  check(fsDepth > 0 && ctx.user == auth.user, "rename keeps guarded caller identity");
  operations.push_back("rename:" + std::string(from.c_str()) + ">" + to.c_str());
  if (denied.count(from.c_str()) || nodes.count(to.c_str()) || !nodes.count(parentOf(to.c_str()))) return false;
  const auto it = nodes.find(from.c_str());
  if (it == nodes.end()) return false;
  auto node = it->second;
  nodes.erase(it);
  // A rename within one directory keeps the entry's listing position.
  if (parentOf(from.c_str()) != parentOf(to.c_str())) {
    auto& siblings = nodes.at(parentOf(from.c_str()))->children;
    siblings.erase(std::remove(siblings.begin(), siblings.end(), node), siblings.end());
    nodes.at(parentOf(to.c_str()))->children.push_back(node);
  }
  node->name = stripSdPrefix(to).c_str();
  nodes[to.c_str()] = node;
  return true;
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
// Operations on the selection act on the entry's full native path, not the
// bounded display name, for cached and beyond-cache indexes alike.
static void selectedOperations(bool beyondCache) {
  resetTree(true);
  add("/sd", "deep", true, "/deep");
  const std::string longFolder(70, 'f'), longName = std::string(75, 'n') + ".txt";
  std::vector<std::string> expected;
  if (beyondCache) {
    for (unsigned i = 0; i < 260; ++i) { add("/sd/deep", numbered(i), false, "/deep/" + numbered(i)); expected.push_back(numbered(i)); }
  }
  add("/sd/deep", longFolder, true, "/deep/" + longFolder);
  add("/sd/deep/" + longFolder, "inside.txt", false, "/deep/" + longFolder + "/inside.txt");
  add("/sd/deep", longName, false, "/deep/" + longName);
  add("/sd/deep", "short.txt", false, "/deep/short.txt");
  expected.push_back(longFolder.substr(0, 63)); expected.push_back(longName.substr(0, 63)); expected.push_back("short.txt");
  const int folderIndex = static_cast<int>(expected.size()) - 3;
  FileManager manager;
  check(manager.navigate("/sd/deep"), "operations fixture loads");
  expectItems(manager, expected);

  for (int i = 0; i < folderIndex; ++i) manager.moveDown();
  check(manager.getSelectedIndex() == folderIndex, "selection reaches the long folder");
  check(manager.navigateInto() && std::string(manager.getCurrentPath()) == "/sd/deep/" + longFolder,
        "entering a long folder name uses the entry's full path");
  expectItems(manager, {"inside.txt"});
#if TEST_SELECTED_PATH
  String selected, content;
  check(manager.getSelectedPath(selected) && selected == String(("/sd/deep/" + longFolder + "/inside.txt").c_str()),
        "selected path is the entry's full native path");
  check(manager.readSelectedFile(content) && content.length() == 12, "selected file reads by its full path");
#endif
  check(manager.navigateUp() && std::string(manager.getCurrentPath()) == "/sd/deep", "parent navigation restored");
  for (int i = 0; i < folderIndex + 1; ++i) manager.moveDown();
  check(manager.getSelectedIndex() == folderIndex + 1, "selection reaches the long file");
#if TEST_SELECTED_PATH
  check(manager.getSelectedPath(selected) && selected == String(("/sd/deep/" + longName).c_str()),
        "beyond-cache or cached long file resolves to its full path");
  check(manager.readSelectedFile(content) && content.length() == 12, "long file reads by its full path");
  check(!manager.readFile(longName.substr(0, 63).c_str(), content), "display-name read cannot reach the long file");
#endif
  check(manager.renameItem("renamed.txt") && nodes.count("/sd/deep/renamed.txt") && !nodes.count("/sd/deep/" + longName),
        "rename addresses the long file by its full path");
  check(manager.getSelectedIndex() == folderIndex + 1, "rename keeps the selection");
  check(manager.deleteItem() && !nodes.count("/sd/deep/renamed.txt") &&
        manager.getItemCount() == static_cast<int>(expected.size()) - 1, "delete addresses the renamed file by its full path");
  manager.moveUp();
  check(manager.getSelectedIndex() == folderIndex && !manager.deleteItem() && nodes.count("/sd/deep/" + longFolder),
        "a populated long folder is refused by the backend, not mis-addressed");
  detach("/sd/deep/" + longFolder + "/inside.txt");
  check(manager.deleteItem() && !nodes.count("/sd/deep/" + longFolder), "empty long folder removed by its full path");
  check(manager.getItemCount() == static_cast<int>(expected.size()) - 2 && fsDepth == 0 && permissionViews == 0,
        "operations release the lock and keep the listing current");
  const std::string tail = operations.empty() ? "" : operations.back();
  check(tail == "rmdir:/sd/deep/" + longFolder, "last backend operation addressed the full folder path");
}
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
  if (selected == "all" || selected == "selected-operations") {
    for (bool beyondCache : {false, true}) {
      run(std::string("selected operations use full paths ") + (beyondCache ? "beyond the cache" : "within the cache"),
          [=]() { selectedOperations(beyondCache); });
    }
  }
  std::printf("FileManager production traversal: %zu assertions, %u failed scenarios\n", checks, failures);
  return failures ? 1 : 0;
}
