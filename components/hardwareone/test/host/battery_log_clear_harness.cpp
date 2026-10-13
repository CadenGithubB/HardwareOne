// Drives the production `batterylog clear` branch of cmd_batterylog
// (System_Battery.cpp) against the shared fake VFS. The filesystem permission
// owner (canDelete) is a double that answers per account: "admin" may delete
// /battery.csv, "user1" may only read it, as the real /battery.csv row says.
// A refused account also gets a refused remove, as the real guarded remove
// would give it.
#define FAKE_VFS_PROVIDE_VFS_NAMESPACE
#include <Arduino.h>
#include "fake_vfs.h"

#include <cstdio>
#include <cstring>
#include <string>

void webBatchTestStringAppend() {}

static char gDebugBuffer[1024];
static char* getDebugBuffer() { return gDebugBuffer; }

static bool gFsLockHeld = false;
static unsigned gFsLockTakes = 0;
static void fsLock(const char*) { gFsLockHeld = true; ++gFsLockTakes; }
static void fsUnlock() { gFsLockHeld = false; }

static unsigned gCanDeleteCalls = 0;
bool canDelete(const String& path, const AuthContext& ctx) {
  ++gCanDeleteCalls;
  return std::string(path.c_str()) == "/battery.csv" && std::string(ctx.user.c_str()) != "user1";
}

// INSERT_CONSTANTS

static const char* clearBranch(const String& a, const AuthContext& ctx) {
  // INSERT_CLEAR_BRANCH
  return nullptr;
}

#define CHECK(condition, what) \
  do { if (!(condition)) { std::fprintf(stderr, "FAIL: %s (reply: %s)\n", what, reply ? reply : "(none)"); return 1; } } while (0)

static void seedLogs() {
  auto& fs = fake_vfs::fs();
  fs.reset();
  fs.lockHeld = [] { return gFsLockHeld; };
  fs.put("/battery.csv", "ts,mv\n1,3900\n");
  fs.put("/battery.csv.1", "ts,mv\n0,3950\n");
  fs.put("/battery.csv.2", "ts,mv\n0,4000\n");
  gCanDeleteCalls = 0;
  gFsLockTakes = 0;
}

static bool anyLogLeft() {
  auto& fs = fake_vfs::fs();
  return fs.exists("/battery.csv") || fs.exists("/battery.csv.1") || fs.exists("/battery.csv.2");
}

static bool allLogsLeft() {
  auto& fs = fake_vfs::fs();
  return fs.exists("/battery.csv") && fs.exists("/battery.csv.1") && fs.exists("/battery.csv.2");
}

static AuthContext account(const char* name) {
  AuthContext ctx{};
  ctx.user = name;
  ctx.path = "cli";
  return ctx;
}

static int run(const std::string& name) {
  const char* reply = nullptr;
  seedLogs();
  if (name == "admin") {
    reply = clearBranch(String("clear"), account("admin"));
    CHECK(reply && std::strcmp(reply, "Battery log cleared.") == 0, "an admin clear reports cleared");
    CHECK(!anyLogLeft(), "the log and both rotations are removed");
    CHECK(fake_vfs::fs().unlockedCalls() == 0, "every filesystem call runs under the FS lock");
  } else if (name == "user_refused") {
    fake_vfs::fs().faults.remove.add("*");  // the guarded remove refuses this account
    reply = clearBranch(String("clear"), account("user1"));
    CHECK(reply && std::strncmp(reply, "Error:", 6) == 0, "a refused clear is reported as an error");
    CHECK(std::strstr(reply, "not cleared") != nullptr, "the reply says the log was not cleared");
    CHECK(allLogsLeft(), "nothing was removed");
  } else if (name == "remove_failed") {
    fake_vfs::fs().faults.remove.add("/battery.csv.1");
    reply = clearBranch(String("clear"), account("admin"));
    CHECK(reply && std::strncmp(reply, "Error:", 6) == 0, "a failed remove is reported as an error");
    CHECK(std::strstr(reply, "not cleared") != nullptr && std::strstr(reply, "1 file") != nullptr,
          "the reply counts the file that stayed");
    CHECK(fake_vfs::fs().exists("/battery.csv.1"), "the failed rotation is still there");
  } else if (name == "nothing_to_clear") {
    fake_vfs::fs().reset();
    fake_vfs::fs().lockHeld = [] { return gFsLockHeld; };
    reply = clearBranch(String("clear"), account("admin"));
    CHECK(reply && std::strcmp(reply, "Battery log cleared.") == 0, "an empty log clears cleanly");
  } else if (name == "other_args") {
    reply = clearBranch(String("tail"), account("admin"));
    CHECK(reply == nullptr && allLogsLeft(), "only the clear argument reaches the clear branch");
  } else {
    CHECK(false, "known scenario");
  }
  (void)gFsLockTakes;
  (void)gCanDeleteCalls;
  std::puts("PASS");
  return 0;
}

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: %s <scenario>\n", argv[0]);
    return 2;
  }
  return run(argv[1]);
}
