// Executes production code that runs beside the command executor on other
// tasks and must not share its per-command state:
//   HW1_CASE_BROADCAST - broadcastOutputCore (System_Debug.cpp) with the
//     validate-state accessor currentCommandValidateOnly (System_Utils.cpp):
//     one thread runs a validate pass the way cmd_exec does, another task
//     broadcasts at the same time; only the validating task's output is
//     dropped.
//   HW1_CASE_PASSWORD - userChangePasswordCore and cmd_user_changepassword
//     (System_User.cpp): the status comes back as the return value and the
//     message in the caller's buffer; the shared debug buffer is untouched.
// Threads, task identity, the per-task command context and the output queue
// are host stand-ins; everything inserted below is the shipping source.
#include "Arduino.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

void webBatchTestStringAppend() {}

static int checks = 0;
#define CHECK(condition) do { \
  if (!(condition)) { std::fprintf(stderr, "CHECK failed at line %d: %s\n", __LINE__, #condition); std::abort(); } \
  ++checks; } while (0)

#if defined(HW1_CASE_BROADCAST)

// INSERT_ROUTES
#define ENABLE_ESPNOW 0

struct CommandContext {
  uint32_t outputMask = 0;
  bool validateOnly = false;
};
struct CaptureBufState { char* buf; size_t len; size_t cap; };
typedef void* TaskHandle_t;

// The executor's handler mirror (HardwareOne.cpp). Set by the validating
// thread exactly as commandExecTask does.
bool gCLIValidateOnly = false;
bool gInHelpRender = false;
bool gThermalRunning = true, gImuRunning = true, gTofRunning = true;
TaskHandle_t gThermalTaskHandle = nullptr, gImuTaskHandle = nullptr, gTofTaskHandle = nullptr;

static thread_local CommandContext* tTaskContext = nullptr;
static thread_local int tTaskIdentity = 0;
void* currentCommandContext() { return tTaskContext; }
CaptureBufState* currentCaptureState() { return nullptr; }
static TaskHandle_t xTaskGetCurrentTaskHandle() { return &tTaskIdentity; }
uint32_t getCurrentCommandOutputMask() { return tTaskContext ? tTaskContext->outputMask : 0; }

static std::mutex deliveredLock;
static std::vector<std::string> delivered;
static bool enqueueChunk(const char* text, size_t len, uint8_t, uint8_t, bool) {
  std::lock_guard<std::mutex> lock(deliveredLock);
  delivered.emplace_back(text, len);
  return true;
}
static bool wasDelivered(const char* text) {
  std::lock_guard<std::mutex> lock(deliveredLock);
  for (const std::string& line : delivered) if (line == text) return true;
  return false;
}

bool currentCommandValidateOnly();
// INSERT_BROADCAST_PRODUCTION

static void say(const char* text) { broadcastOutputCore(text, std::strlen(text), 0); }

int main() {
  std::promise<void> validating, otherTaskDone;
  std::future<void> otherTaskDoneFuture = otherTaskDone.get_future();

  // cmd_exec running a validate=1 request.
  std::thread executor([&] {
    CommandContext dryRun;
    dryRun.validateOnly = true;
    dryRun.outputMask = MSG_ROUTE_WEB;
    tTaskContext = &dryRun;
    const bool previous = gCLIValidateOnly;
    gCLIValidateOnly = dryRun.validateOnly;
    say("executor: dry-run output");
    validating.set_value();
    otherTaskDoneFuture.wait();
    say("executor: still validating");
    gCLIValidateOnly = previous;
    tTaskContext = nullptr;
    say("executor: after the validate pass");
  });

  validating.get_future().wait();
  // A sensor task or httpd broadcasting while that validate pass runs.
  say("other task: ambient line");
  CommandContext realCommand;
  realCommand.outputMask = MSG_ROUTE_SERIAL;
  tTaskContext = &realCommand;
  say("other task: its own command output");
  tTaskContext = nullptr;
  otherTaskDone.set_value();
  executor.join();

  CHECK(!wasDelivered("executor: dry-run output"));
  CHECK(!wasDelivered("executor: still validating"));
  CHECK(wasDelivered("other task: ambient line"));
  CHECK(wasDelivered("other task: its own command output"));
  CHECK(wasDelivered("executor: after the validate pass"));
  std::printf("command task isolation: broadcast ok\n");
  std::fprintf(stderr, "checks=%d\n", checks);
  return 0;
}

#elif defined(HW1_CASE_PASSWORD)

enum CommandSource { SOURCE_INTERNAL, SOURCE_WEB, SOURCE_SERIAL };
struct AuthContext {
  CommandSource transport = SOURCE_WEB;
  String user, ip, path, sid;
};
enum { SYSEVT_PASSWORD_CHANGED = 1 };

// INSERT_CONSTANTS

bool gCLIValidateOnly = false;
bool filesystemReady = true;
static AuthContext installed;
static std::string storedPassword;
static bool storageOk = true;
static int revokes = 0, events = 0;

String currentExecUser() { return installed.user; }
const AuthContext& currentAuthContext() { return installed; }
bool isValidUser(const String& user, const String& password) {
  return user == installed.user && storedPassword == password.c_str();
}
bool setUserPassword(const String&, const String& password) {
  if (!storageOk) return false;
  storedPassword = password.c_str();
  return true;
}
template <typename... Args> void logAuthAttempt(Args&&...) {}
template <typename... Args> void revokeUserSessions(Args&&...) { ++revokes; }
template <typename... Args> void systemEventPost(Args&&...) { ++events; }

// The shared debug buffer, holding another command's result.
static char debugBuffer[4096];
static int debugBufferUses = 0;
static const char kOtherResult[] = "another user's command result";
static bool ensureDebugBuffer() { ++debugBufferUses; return true; }
static char* getDebugBuffer() { ++debugBufferUses; return debugBuffer; }

class CommandArgs {
 public:
  explicit CommandArgs(const String& input) {
    std::string text(input.c_str()), token;
    for (char c : text + " ") {
      if (c == ' ') { if (!token.empty()) args_.emplace_back(token); token.clear(); }
      else token += c;
    }
  }
  const String& arg(int index) const {
    static const String empty;
    return index >= 0 && index < static_cast<int>(args_.size()) ? args_[index] : empty;
  }
  bool hasMinArgs(int n) const { return static_cast<int>(args_.size()) >= n; }
 private:
  std::vector<String> args_;
};

// INSERT_PASSWORD_PRODUCTION

static void reset(const char* user) {
  installed = AuthContext();
  installed.user = user;
  installed.path = "/password-change";
  storedPassword = "oldpass1";
  storageOk = true;
  revokes = events = 0;
  std::snprintf(debugBuffer, sizeof(debugBuffer), "%s", kOtherResult);
  debugBufferUses = 0;
}

static bool change(const char* current, const char* next, const char* confirm, std::string& message) {
  char buffer[USER_CHANGE_PASSWORD_MESSAGE_MAX];
  std::memset(buffer, 'x', sizeof(buffer));
  const bool ok = userChangePasswordCore(current, next, confirm, buffer, sizeof(buffer));
  CHECK(std::memchr(buffer, '\0', sizeof(buffer)) != nullptr);
  message = buffer;
  // Never the shared buffer: another task may be formatting into it.
  CHECK(debugBufferUses == 0);
  CHECK(std::strcmp(debugBuffer, kOtherResult) == 0);
  return ok;
}

int main() {
  std::string message;

  reset("alice");
  CHECK(!change("oldpass1", "newpass1", "different", message));
  CHECK(message == "Error: New passwords do not match");
  CHECK(!change("oldpass1", "short", "short", message));
  CHECK(message == "Error: Password must be at least 6 characters");
  CHECK(!change("oldpass1", "oldpass1", "oldpass1", message));
  CHECK(message == "Error: New password must differ from current password");
  CHECK(!change("wrongpass", "newpass1", "newpass1", message));
  CHECK(message == "Error: Current password incorrect");
  CHECK(storedPassword == "oldpass1");

  filesystemReady = false;
  CHECK(!change("oldpass1", "newpass1", "newpass1", message));
  CHECK(message == "Error: LittleFS not ready");
  filesystemReady = true;

  reset("");
  CHECK(!change("oldpass1", "newpass1", "newpass1", message));
  CHECK(message == "Error: Not authenticated");

  reset("alice");
  storageOk = false;
  CHECK(!change("oldpass1", "newpass1", "newpass1", message));
  CHECK(message == "Error: Failed to change password for user 'alice'");
  CHECK(revokes == 0 && events == 0);

  reset("alice");
  CHECK(change("oldpass1", "newpass1", "newpass1", message));
  CHECK(message == "Password changed successfully for user 'alice'");
  CHECK(storedPassword == "newpass1");
  CHECK(revokes == 1 && events == 1);

  // The longest username (64 characters) still fits the advertised size.
  const std::string longName(64, 'n');
  reset(longName.c_str());
  storageOk = false;
  CHECK(!change("oldpass1", "newpass1", "newpass1", message));
  CHECK(message == "Error: Failed to change password for user '" + longName + "'");
  reset(longName.c_str());
  CHECK(change("oldpass1", "newpass1", "newpass1", message));
  CHECK(message == "Password changed successfully for user '" + longName + "'");

  // A short buffer is cut and terminated; no buffer still reports the status.
  reset("alice");
  std::vector<char> tiny(8, 'x');
  CHECK(userChangePasswordCore("oldpass1", "newpass1", "newpass1", tiny.data(), tiny.size()));
  CHECK(std::string(tiny.data()) == "Passwor");
  reset("alice");
  CHECK(!userChangePasswordCore("oldpass1", "a", "b", nullptr, 0));
  reset("alice");
  CHECK(userChangePasswordCore("oldpass1", "newpass1", "newpass1", nullptr, 0));
  CHECK(storedPassword == "newpass1");

  // The CLI handler runs on the executor and still answers in the handler
  // convention (its reply in the debug buffer).
  reset("alice");
  const char* reply = cmd_user_changepassword("oldpass1 newpass1 newpass1");
  CHECK(reply == debugBuffer);
  CHECK(std::strcmp(reply, "Password changed successfully for user 'alice'") == 0);
  reset("alice");
  reply = cmd_user_changepassword("oldpass1 newpass1 other");
  CHECK(std::strcmp(reply, "Error: New passwords do not match") == 0);
  reset("alice");
  CHECK(std::strncmp(cmd_user_changepassword("oldpass1"), "Error: invalid arguments", 24) == 0);
  gCLIValidateOnly = true;
  CHECK(std::strcmp(cmd_user_changepassword("oldpass1 newpass1 newpass1"), "VALID") == 0);
  CHECK(storedPassword == "oldpass1");
  gCLIValidateOnly = false;

  std::printf("command task isolation: password ok\n");
  std::fprintf(stderr, "checks=%d\n", checks);
  return 0;
}

#else
#error "define HW1_CASE_BROADCAST or HW1_CASE_PASSWORD"
#endif
