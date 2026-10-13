// The production filecreate command handler is inserted below. The argument
// tokenizer, auth context, FS lock and VFS are controlled doubles: the VFS
// double keeps a name -> content store and gives "w" the same create-or-
// truncate meaning the real VFS::open has, so a truncating create shows up as
// lost content. Permission rules, real LittleFS/SD drivers and task races are
// not modelled.
#include <cstdio>
#include <cstring>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

static unsigned assertions = 0;
static void check(bool condition, const char* reason) {
  ++assertions;
  if (!condition) throw std::runtime_error(reason);
}

class String {
  std::string text_;
 public:
  String() = default;
  String(const char* s) : text_(s ? s : "") {}
  String(const std::string& s) : text_(s) {}
  size_t length() const { return text_.size(); }
  const char* c_str() const { return text_.c_str(); }
  const std::string& str() const { return text_; }
  bool endsWith(const String& s) const {
    return length() >= s.length() && text_.compare(length() - s.length(), s.length(), s.text_) == 0;
  }
  friend bool operator==(const String& a, const String& b) { return a.text_ == b.text_; }
};

static std::vector<String> tokens;
static bool quotedPath = true;
class CommandArgs {
 public:
  explicit CommandArgs(const String&) {}
  bool has(int index) const { return index >= 0 && (size_t)index < tokens.size(); }
  const String& arg(int index) const { return tokens.at((size_t)index); }
};
static const char* requireQuotedPath(const CommandArgs& args, int index, String& path) {
  if (!quotedPath || !args.has(index)) return "Error: path must be a quoted token";
  path = args.arg(index);
  return nullptr;
}
struct AuthContext { int user = 1; };
static AuthContext auth;
static const AuthContext& currentAuthContext() { return auth; }
static bool filesystemReady = true, validationOnly = false;
#define RETURN_VALID_IF_VALIDATE_CSTR() do { if (validationOnly) return "VALID"; } while (false)
static char debugBuffer[1024];
static bool ensureDebugBuffer() { return true; }
static char* getDebugBuffer() { return debugBuffer; }

static unsigned fsDepth = 0, existsCalls = 0, openCalls = 0, closeCalls = 0;
static bool permissionDenied = false;
static std::map<std::string, std::string> store;
struct FsLockGuard {
  explicit FsLockGuard(const char* name) { check(std::strcmp(name, "filecreate") == 0, "FS guard tag"); ++fsDepth; }
  ~FsLockGuard() { --fsDepth; }
};
class File {
 public:
  explicit File(bool valid) : valid_(valid) {}
  explicit operator bool() const { return valid_; }
  void close() { check(fsDepth == 1, "file closes under the FS lock"); ++closeCalls; valid_ = false; }
 private:
  bool valid_;
};
namespace VFS {
// maybe_unused keeps older handler versions that never probe compiling, so the
// content assertion below is what reports a truncating create.
[[maybe_unused]] static bool existsGuarded(const String& path, const AuthContext& ctx) {
  check(fsDepth == 1, "existence probe runs under the FS lock");
  check(&ctx == &auth, "existence probe keeps the caller identity");
  ++existsCalls;
  if (permissionDenied) return false;
  return store.count(path.str()) != 0;
}
static File openGuarded(const String& path, const char* mode, const AuthContext& ctx, bool create) {
  check(fsDepth == 1, "guarded open runs under the FS lock");
  check(&ctx == &auth, "guarded open keeps the caller identity");
  check(std::strcmp(mode, "w") == 0 && create, "create opens with the write/create mode");
  ++openCalls;
  if (permissionDenied) return File(false);
  store[path.str()] = "";  // "w" creates or truncates, like the real VFS::open
  return File(true);
}
}  // namespace VFS

// INSERT_PRODUCTION_FILECREATE_HERE

static std::string invoke(std::initializer_list<const char*> args) {
  tokens.clear();
  for (const char* arg : args) tokens.emplace_back(arg);
  const char* reply = cmd_filecreate(String("mock tokenization boundary"));
  check(fsDepth == 0, "every handler path releases the FS lock");
  check(reply != nullptr && std::strlen(reply) < sizeof(debugBuffer), "reply fits the command buffer");
  return reply;
}
static void fixture() {
  store.clear();
  store["/notes.txt"] = "keep this content";
  quotedPath = true; filesystemReady = true; validationOnly = false; permissionDenied = false;
}

int main() {
  try {
    // An existing file keeps its content: create refuses instead of truncating.
    fixture();
    unsigned opens = openCalls;
    std::string reply = invoke({"/notes.txt"});
    check(reply.rfind("Error:", 0) == 0, "creating over an existing file is an error");
    check(reply.find("already exists") != std::string::npos && reply.find("/notes.txt") != std::string::npos,
          "the error names the collision and the path");
    check(store.at("/notes.txt") == "keep this content", "existing content survives a repeated create");
    check(openCalls == opens, "no truncating open happens for an existing path");
    check(existsCalls > 0, "the existence probe runs before the open");

    // The normal create path still works and leaves the neighbour alone.
    fixture();
    opens = openCalls;
    const unsigned closes = closeCalls;
    check(invoke({"/new.txt"}) == "Created file: /new.txt", "a new file reports the created path");
    check(store.count("/new.txt") == 1 && store.at("/new.txt").empty(), "a new empty file is created");
    check(openCalls == opens + 1 && closeCalls == closes + 1, "the create opens once and closes the handle");
    check(store.at("/notes.txt") == "keep this content", "other files are untouched by a create");

    // Denied or failed opens keep their established reply.
    fixture();
    permissionDenied = true;
    check(invoke({"/secret/x.txt"}) == "Error: Failed to create file (denied or fs error): /secret/x.txt",
          "denied create keeps the established error");
    check(store.count("/secret/x.txt") == 0, "denied create writes nothing");

    // Early argument errors never reach the VFS.
    fixture();
    const unsigned exists = existsCalls;
    opens = openCalls;
    check(invoke({"/dir/"}) == "Error: Path must be a file (not a directory)", "trailing slash is rejected");
    check(invoke({"/a.txt", "extra"}) == "Error: unexpected argument — usage: filecreate \"<path>\"",
          "extra argument is rejected");
    quotedPath = false;
    check(invoke({"/a.txt"}) == "Error: path must be a quoted token", "unquoted path is rejected");
    quotedPath = true; filesystemReady = false;
    check(invoke({"/a.txt"}) == "Error: LittleFS not ready", "not-ready literal remains unchanged");
    filesystemReady = true; validationOnly = true;
    check(invoke({"/a.txt"}) == "VALID", "validate-only returns without touching the VFS");
    check(existsCalls == exists && openCalls == opens, "early errors do not touch the VFS");
    check(store.size() == 1 && store.at("/notes.txt") == "keep this content", "early errors change no file");

    std::cout << "filecreate: " << assertions << " checks passed\n";
  } catch (const std::exception& error) {
    std::cerr << "filecreate harness failed after " << assertions << " checks: " << error.what() << '\n';
    return 1;
  }
}
