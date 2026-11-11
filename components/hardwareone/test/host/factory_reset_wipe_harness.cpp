// Runs the production factoryreset confirm callback and prompt from
// System_Utils.cpp on the shared fake VFS (fake_vfs.h), with the real
// FsLockGuard and the real VFS DirectoryReader. The microphone drain, the
// ramflush overlay, the reboot timer, events, logs and the System_User
// account-id record are doubles that only count their calls.
#define FAKE_VFS_PROVIDE_VFS_NAMESPACE
#define FAKE_VFS_LOCK_DOUBLE
#include <Arduino.h>
#include "fake_vfs.h"

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

void webBatchTestStringAppend() {}

static unsigned checks = 0;
static void checked(bool ok, const char* text, int line) {
  ++checks;
  if (ok) return;
  std::fprintf(stderr, "factory reset check failed at line %d: %s\n", line, text);
  std::exit(1);
}
#define check(condition) checked((condition), #condition, __LINE__)

// INSERT_FS_GUARD

namespace VFS {
// INSERT_DIRECTORY_READER
}  // namespace VFS

// ---------------------------------------------------------------------------
// Doubles for what the callback reaches outside the filesystem.
// ---------------------------------------------------------------------------
#ifndef ENABLE_BLUETOOTH
#define ENABLE_BLUETOOTH 1
#endif
// The pipe secret is a device code (`phone pair new` marked it, phone link v2).
static struct {String phonePipeSecret="fixture-secret";bool phonePipeSecretGenerated=true;} gSettings;
static int pipeFailure=0;static bool keyCleared=false,enrollmentCleared=false,secretPersisted=false,generatedPersisted=true;
// The BLE role token (Bluetooth.h): the reset holds PHONE_STOP from the pipe's
// stop through the secret's clear, so no link start takes the old secret; it
// is refused while another task holds the token. outsideRole counts the stop,
// key drop and clear made without it.
enum class BleRoleTransition : uint8_t { IDLE = 0, PHONE_STOP };
static bool roleBusy=false;static unsigned roleDepth=0,outsideRole=0;static int roleKind=-1;
static bool bleRoleTransitionBegin(BleRoleTransition kind){if(roleBusy)return false;if(!roleDepth++)roleKind=int(kind);return true;}
static void bleRoleTransitionEnd(){--roleDepth;}
static bool phonePipeStop(){if(!roleDepth)++outsideRole;return pipeFailure!=1;}
static bool phonePipeInvalidateSecret(){if(!roleDepth)++outsideRole;keyCleared=pipeFailure!=2;return keyCleared;}
static bool setInternalSecretSetting(String& field,const String& value){
  if(!roleDepth)++outsideRole;if(pipeFailure==3)return false;field=value;return true;}
static bool blePeerClearPhonePipeEnrollment(){enrollmentCleared=pipeFailure!=4;return enrollmentCleared;}
static bool writeSettingsJsonForFactoryReset(){
  generatedPersisted=gSettings.phonePipeSecretGenerated;secretPersisted=pipeFailure!=5;return secretPersisted;}
#define EXT_RAM_BSS_ATTR
#define RETURN_VALID_IF_VALIDATE_CSTR() ((void)0)
const char* USERS_JSON_FILE = "/system/users/users.json";

static bool micFinalizeOk = true;
static bool finalizeMicRecordingForRestart(const char*) { return micFinalizeOk; }
static unsigned overlayClears = 0;
static void ramFlushClearOverlay() { ++overlayClears; }

struct FakeEsp { unsigned restarts = 0; void restart() { ++restarts; } };
static FakeEsp ESP;

struct FakeTimer { int unused; };
typedef FakeTimer* esp_timer_handle_t;
typedef void (*esp_timer_cb_t)(void*);
enum esp_timer_dispatch_t { ESP_TIMER_TASK = 0 };
struct esp_timer_create_args_t {
  esp_timer_cb_t callback;
  void* arg;
  esp_timer_dispatch_t dispatch_method;
  const char* name;
  bool skip_unhandled_events;
};
static FakeTimer timerObject;
static unsigned timersCreated = 0, timersStarted = 0;
static uint64_t timerDelayUs = 0;
static esp_timer_cb_t timerCallback = nullptr;
static int esp_timer_create(const esp_timer_create_args_t* args, esp_timer_handle_t* out) {
  ++timersCreated;
  timerCallback = args->callback;
  *out = &timerObject;
  return 0;
}
static int esp_timer_start_once(esp_timer_handle_t, uint64_t us) {
  ++timersStarted;
  timerDelayUs = us;
  return 0;
}

static unsigned rebootIntents = 0;
static void recordRebootIntent(const char*, const char*) { ++rebootIntents; }
enum { SYSEVT_FACTORY_RESET = 77 };
static std::vector<std::string> events, logLines;
static void systemEventPost(int kind, const char* subject = nullptr) {
  events.push_back(std::to_string(kind) + ":" + (subject ? subject : ""));
}
static void logSystemEvent(const char* category, const char* format, ...) {
  char line[512];
  va_list args;
  va_start(args, format);
  std::vsnprintf(line, sizeof(line), format, args);
  va_end(args);
  logLines.push_back(std::string(category) + ": " + line);
}
static const String& currentExecUser() {
  static const String user("fixture-owner");
  return user;
}

// System_User's account-id record: counts calls and notes where in the
// filesystem call log it ran, so the suite can order it against the removals.
static unsigned idRecords = 0;
static size_t idRecordAt = 0;
static bool idRecordOk = true;
bool recordAccountIdHighWater(String& errorOut) {
  ++idRecords;
  idRecordAt = fake_vfs::fs().log.size();
  if (!idRecordOk) errorOut = "fixture refusal";
  return idRecordOk;
}

// `factoryreset all` (System_FactoryWipe.h): the request for the boot-time
// wipe, the OTA probation gate and the transport the command arrived on.
static bool wipeArmed = false, wipeArmOk = true, otaPending = false;
static unsigned wipeArms = 0, wipeDisarms = 0;
static size_t wipeArmAt = 0;
static bool factoryWipeArm() {
  ++wipeArms;
  wipeArmAt = fake_vfs::fs().log.size();
  if (wipeArmOk) wipeArmed = true;
  return wipeArmOk;
}
static void factoryWipeDisarm() { ++wipeDisarms; wipeArmed = false; }
static bool otaSafetyIsPendingVerification() { return otaPending; }
enum CommandOrigin { ORIGIN_SERIAL, ORIGIN_WEB };
struct CommandContext { CommandOrigin origin = ORIGIN_WEB; };
static CommandContext commandContext;
static bool haveCommandContext = true;
static void* currentCommandContext() { return haveCommandContext ? &commandContext : nullptr; }
// Both forms of the handover prompt compile across the two builds.
#ifndef HW1_RADIO_COMPANION
#define HW1_RADIO_COMPANION ENABLE_BLUETOOTH
#endif

// The confirm framework: captures the prompt, the audit command line and the
// mode token handed to the callbacks.
typedef const char* (*CliConfirmCallback)(void* userData);
typedef void (*CliConfirmAcceptedCallback)(void* acceptedData);
static std::string capturedPrompt, capturedCommand;
static void* capturedUserData = nullptr;
static unsigned confirmRequests = 0;
static bool cliRequestConfirm(const String& prompt, const String& command, CliConfirmCallback, CliConfirmCallback,
                              void* userData, CliConfirmAcceptedCallback = nullptr, void* = nullptr) {
  ++confirmRequests;
  capturedPrompt = prompt.c_str();
  capturedCommand = command.c_str();
  capturedUserData = userData;
  return true;
}
static const char* cliConfirmPromptResponse() { return "prompt"; }

// The production code builds the reboot timer with designated initializers
// (C++20 syntax the firmware toolchain accepts); allow just that here.
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc++20-designator"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#endif
// INSERT_FACTORY_RESET
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

// ---------------------------------------------------------------------------
// Fixtures
// ---------------------------------------------------------------------------
static const char* kPending = "/system/users/pending_users.json";
static const char* kSettingsDir = "/system/users/user_settings";

static const char* kRemoved[] = {
    "/system/users/users.json",
    "/system/users/pending_users.json",
    "/system/users/user_settings/1.json",
    "/system/users/user_settings/2.json",
    "/system/users/user_settings/3.json.tmp",
    // Copies kept of a damaged or partly read roster or pending list.
    "/system/users/users.json.corrupt-1",
    "/system/users/users.json.corrupt-2",
    "/system/users/users.json.partial-1",
    "/system/users/users.json.bad",
    "/system/users/users.json.corrupt",
    "/system/users/pending_users.json.corrupt-1",
    "/stt/u1",
    "/stt/u1/2026-01-01-local.txt",
    "/stt/u2",
    "/stt/u2/notes",
    "/stt/u2/notes/n1.txt",
    "/stt/u2/notes/sub/deeper.txt",
    "/stt/u3",
    "/stt/u12/x.txt",
};
static const char* kSdRemoved[] = {
    "/sd/stt/u1/b.txt",
    "/sd/stt/u4",
    "/sd/stt/u4/notes/n.txt",
};
static const char* kKept[] = {
    "/system/settings.json",
    "/system/users/ip_bans.json",
    "/system/users/ip_bans.json.corrupt-1",
    "/system/users/user_settings",
    "/stt",
    "/stt/models/keep.bin",
    "/stt/ufoo/keep.txt",
    "/stt/u5.txt",
    "/sd/stt",
    "/sd/stt/shared.txt",
    "/sd/VIDEOS/v.avi",
};

static fake_vfs::Fs& populate() {
  auto& fs = fake_vfs::fs();
  fs.reset();
  fake_vfs::wireLockDouble();
  gFakeFsSemaphore = FakeFsSemaphore{};
  fs.put("/system/settings.json", "{}");
  fs.put("/system/users/users.json", "{\"nextId\":5,\"users\":[{\"id\":1,\"username\":\"owner\"}]}");
  fs.put(kPending, "[{\"username\":\"erin\",\"password\":\"hash:x\"}]");
  fs.put("/system/users/ip_bans.json", "[]");
  fs.put("/system/users/ip_bans.json.corrupt-1", "[");
  for (const char* kept : {"users.json.corrupt-1", "users.json.corrupt-2", "users.json.partial-1", "users.json.bad",
                           "users.json.corrupt", "pending_users.json.corrupt-1"}) {
    fs.put(std::string("/system/users/") + kept, "{\"password\":\"hash:old\"");
  }
  fs.put("/system/users/user_settings/1.json", "{\"password\":\"hash:1\"}");
  fs.put("/system/users/user_settings/2.json", "{\"password\":\"hash:2\"}");
  fs.put("/system/users/user_settings/3.json.tmp", "{");
  fs.put("/stt/u1/2026-01-01-local.txt", "owner transcript");
  fs.put("/stt/u2/notes/n1.txt", "private note");
  fs.put("/stt/u2/notes/sub/deeper.txt", "deeper");
  fs.mkdirAll("/stt/u3");
  fs.put("/stt/u12/x.txt", "x");
  fs.put("/stt/models/keep.bin", "model");
  fs.put("/stt/ufoo/keep.txt", "not an account folder");
  fs.put("/stt/u5.txt", "a file, not a folder");
  fs.put("/sd/stt/u1/b.txt", "sd transcript");
  fs.put("/sd/stt/u4/notes/n.txt", "sd note");
  fs.put("/sd/stt/shared.txt", "shared");
  fs.put("/sd/VIDEOS/v.avi", "video");
  pipeFailure=0;keyCleared=enrollmentCleared=secretPersisted=false;gSettings.phonePipeSecret="fixture-secret";
  roleBusy=false;roleDepth=outsideRole=0;roleKind=-1;
  gSettings.phonePipeSecretGenerated=generatedPersisted=true;
  micFinalizeOk = true; overlayClears = 0;
  ESP = FakeEsp{};
  timersCreated = timersStarted = 0; timerDelayUs = 0; timerCallback = nullptr;
  rebootIntents = 0; events.clear(); logLines.clear();
  idRecords = 0; idRecordAt = 0; idRecordOk = true;
  wipeArmed = false; wipeArmOk = true; otaPending = false; wipeArms = wipeDisarms = 0; wipeArmAt = 0;
  commandContext.origin = ORIGIN_WEB; haveCommandContext = true;
  capturedPrompt.clear(); capturedCommand.clear(); capturedUserData = nullptr; confirmRequests = 0;
  return fs;
}

static bool startsWith(const char* text, const char* prefix) {
  return std::strncmp(text, prefix, std::strlen(prefix)) == 0;
}

static size_t firstCall(const char* op, const std::string& prefix, size_t from) {
  const auto& log = fake_vfs::fs().log;
  for (size_t i = from; i < log.size(); ++i) {
    if (log[i].op == op && log[i].ok && log[i].path.compare(0, prefix.size(), prefix) == 0) return i;
  }
  return SIZE_MAX;
}

static size_t lastCall(const char* op, const std::string& prefix, size_t from) {
  const auto& log = fake_vfs::fs().log;
  size_t found = SIZE_MAX;
  for (size_t i = from; i < log.size(); ++i) {
    if (log[i].op == op && log[i].ok && log[i].path.compare(0, prefix.size(), prefix) == 0) found = i;
  }
  return found;
}

static bool committed() { return timersCreated == 1 && timersStarted == 1 && events.size() == 1; }
static bool notCommitted() { return timersCreated == 0 && timersStarted == 0 && events.empty(); }

// ---------------------------------------------------------------------------
// Cases
// ---------------------------------------------------------------------------
static void wipeRemovesAccountData() {
  auto& fs = populate();
  const size_t start = fs.log.size();
  const char* reply = factoryreset_confirmed(nullptr);
  check(startsWith(reply, "Factory reset complete"));
  check(std::strstr(reply, "SD card") == nullptr);
  for (const char* path : kRemoved) check(!fs.exists(path));
  for (const char* path : kSdRemoved) check(!fs.exists(path));
  for (const char* path : kKept) check(fs.exists(path));
  check(fs.isDirectory(kSettingsDir) && fs.isDirectory("/stt") && fs.isDirectory("/sd/stt"));
  check(committed() && timerDelayUs == 1000000ULL && timerCallback != nullptr && rebootIntents == 1);
  check(events[0] == std::to_string(SYSEVT_FACTORY_RESET) + ":fixture-owner");
  check(overlayClears == 1 && ESP.restarts == 0);
  check(secretPersisted&&!gSettings.phonePipeSecret.length());
  check(!gSettings.phonePipeSecretGenerated&&!generatedPersisted);  // no device code is left
#if ENABLE_BLUETOOTH
  check(keyCleared&&enrollmentCleared);
  // The role was held at the stop, the key drop and the clear, and given back.
  check(outsideRole==0&&roleDepth==0&&roleKind==int(BleRoleTransition::PHONE_STOP));
#endif
  timerCallback(nullptr);
  check(ESP.restarts == 1);

  // The issued ids are recorded before anything is removed, the account data
  // goes before users.json (the commit), and the settings files after it.
  check(idRecords == 1 && idRecordAt <= firstCall("remove", "/", start));
  const size_t roster = firstCall("remove", USERS_JSON_FILE, start);
  check(roster != SIZE_MAX);
  check(firstCall("remove", kPending, start) < roster);
  check(lastCall("rmdir", "/stt/", start) < roster && lastCall("rmdir", "/sd/stt/", start) < roster);
  check(firstCall("remove", std::string(kSettingsDir) + "/", start) > roster);
  check(firstCall("remove", "/system/users/users.json.", start) > roster);
  // Every filesystem call of the wipe ran under the filesystem lock and as system.
  for (size_t i = start; i < fs.log.size(); ++i) {
    check(fs.log[i].locked);
    if (fs.log[i].guarded) check(fs.log[i].authUser == "system");
  }
  check(fs.liveHandles() == 0);
  // The plain reset never asks for the boot-time wipe.
  check(wipeArms == 0 && wipeDisarms == 0 && !wipeArmed);
  check(std::strstr(reply, "setup wizard") != nullptr && std::strstr(reply, "NVS") == nullptr);
}

static void sdNotMountedKeepsSdFolders() {
  auto& fs = populate();
  fs.sdAvailable() = false;
  const char* reply = factoryreset_confirmed(nullptr);
  check(startsWith(reply, "Factory reset complete") && std::strstr(reply, "SD card not mounted") != nullptr);
  for (const char* path : kRemoved) check(!fs.exists(path));
  for (const char* path : kSdRemoved) check(fs.exists(path));
  check(committed());
  check(fs.liveHandles() == 0);
}

static void accountDataFailureStopsBeforeRoster() {
  auto& fs = populate();
  fs.faults.remove.add("/stt/u2/notes/n1.txt");
  const char* reply = factoryreset_confirmed(nullptr);
  check(startsWith(reply, "Error: factory reset stopped") && std::strstr(reply, "/stt/u2/notes/n1.txt") != nullptr);
  check(fs.exists(USERS_JSON_FILE) && fs.exists("/system/users/user_settings/1.json"));
  check(fs.exists("/system/users/user_settings/2.json") && fs.exists("/stt/u2/notes/n1.txt"));
  check(notCommitted() && rebootIntents == 0);

  // A pending list that cannot be removed stops the reset before any
  // transcript is touched.
  populate();
  fs.faults.remove.add(kPending);
  reply = factoryreset_confirmed(nullptr);
  check(startsWith(reply, "Error: factory reset stopped") && std::strstr(reply, kPending) != nullptr);
  check(fs.exists(USERS_JSON_FILE) && fs.exists(kPending) && fs.exists("/stt/u1/2026-01-01-local.txt"));
  check(fs.exists("/sd/stt/u1/b.txt") && notCommitted());

  // An SD account folder that cannot be removed stops it too.
  populate();
  fs.faults.rmdir.add("/sd/stt/u4/notes");
  reply = factoryreset_confirmed(nullptr);
  check(startsWith(reply, "Error: factory reset stopped") && fs.exists(USERS_JSON_FILE) && notCommitted());

  // users.json that cannot be removed: no reboot, settings files untouched.
  populate();
  fs.faults.remove.add(USERS_JSON_FILE);
  reply = factoryreset_confirmed(nullptr);
  check(startsWith(reply, "Error: factory reset failed") && fs.exists(USERS_JSON_FILE));
  check(fs.exists("/system/users/user_settings/1.json") && notCommitted());
  check(fs.liveHandles() == 0);
}

static void leftoverSettingsStillReboots() {
  auto& fs = populate();
  fs.faults.remove.add("/system/users/user_settings/2.json");
  const char* reply = factoryreset_confirmed(nullptr);
  check(startsWith(reply, "Factory reset complete") && std::strstr(reply, "user settings files") != nullptr);
  check(!fs.exists(USERS_JSON_FILE) && fs.exists("/system/users/user_settings/2.json"));
  check(committed());
  bool logged = false;
  for (const std::string& line : logLines) logged = logged || line.find("user_settings/2.json") != std::string::npos;
  check(logged);

  // A kept copy that cannot be removed is named too; the reset still commits.
  populate();
  fs.faults.remove.add("/system/users/users.json.corrupt-2");
  reply = factoryreset_confirmed(nullptr);
  check(startsWith(reply, "Factory reset complete") && std::strstr(reply, "kept copy") != nullptr);
  check(!fs.exists(USERS_JSON_FILE) && fs.exists("/system/users/users.json.corrupt-2") && committed());
  check(!fs.exists("/system/users/users.json.corrupt-1"));

  // A refused id record is logged; the wipe still completes.
  populate();
  idRecordOk = false;
  reply = factoryreset_confirmed(nullptr);
  check(startsWith(reply, "Factory reset complete") && !fs.exists(USERS_JSON_FILE) && committed());
  check(fs.liveHandles() == 0);
}

static void micDrainFailureTouchesNothing() {
  auto& fs = populate();
  micFinalizeOk = false;
  const size_t start = fs.log.size();
  const char* reply = factoryreset_confirmed(nullptr);
  check(startsWith(reply, "Error: factory reset aborted"));
  check(fs.log.size() == start && notCommitted() && idRecords == 0);
}

static void promptNamesWhatIsDeleted() {
  populate();
  check(std::strcmp(cmd_factoryreset(String("")), "prompt") == 0);
  check(capturedPrompt.find("users.json") != std::string::npos);
  check(capturedPrompt.find("pending") != std::string::npos);
  check(capturedPrompt.find("settings") != std::string::npos);
  check(capturedPrompt.find("/stt/u*") != std::string::npos && capturedPrompt.find("/sd/stt/u*") != std::string::npos);
  // The plain reset says what it keeps, names the full one, and passes no mode.
  check(capturedPrompt.find("PRESERVED") != std::string::npos);
  check(capturedPrompt.find("factoryreset all") != std::string::npos);
  check(capturedPrompt.find("cannot be undone") != std::string::npos);
  check(capturedCommand == "factoryreset" && capturedUserData == nullptr);
  check(capturedPrompt.size() < CONFIRM_PROMPT_SIZE);  // shown whole by the confirm framework
}

// `factoryreset all`: asked only on the serial console and outside OTA
// probation; the prompt lists what the boot-time wipe erases and what it
// leaves, and fits the confirm framework's buffer.
static void fullWipePromptAndGates() {
  populate();
  check(startsWith(cmd_factoryreset(String("everything")), "Usage: factoryreset [all]") && confirmRequests == 0);
  check(startsWith(cmd_factoryreset(String("all now")), "Usage:") && confirmRequests == 0);
  check(startsWith(cmd_factoryreset(String("all")), "Error: factoryreset all is available only on the physical serial"));
  haveCommandContext = false;
  check(startsWith(cmd_factoryreset(String("all")), "Error: factoryreset all is available only"));
  haveCommandContext = true; commandContext.origin = ORIGIN_SERIAL; otaPending = true;
  check(startsWith(cmd_factoryreset(String("all")), "Error: factoryreset all refused while the new firmware"));
  check(confirmRequests == 0 && wipeArms == 0);

  otaPending = false;
  check(std::strcmp(cmd_factoryreset(String("  ALL ")), "prompt") == 0 && confirmRequests == 1);
  check(capturedCommand == "factoryreset all" && capturedUserData != nullptr);
  for (const char* named : {"users.json", "/stt/u*", "/sd/stt/u*", "filesystem", "NVS", "WiFi", "MQTT", "ESP-NOW",
                            "Bluetooth bonds", "otapin", "Kept: firmware", "SD card", "resumes if power is lost",
                            "cannot be undone"}) {
    check(capturedPrompt.find(named) != std::string::npos);
  }
  check((capturedPrompt.find("radio companion") != std::string::npos) == (HW1_RADIO_COMPANION != 0));
  check(capturedPrompt.find("PRESERVED") == std::string::npos);
  check(capturedPrompt.size() < CONFIRM_PROMPT_SIZE);
  check(wipeArms == 0);  // nothing is requested before the answer
}

// Confirmed: the wipe is requested before anything is removed and stays
// requested once the accounts are gone.
static void fullWipeRequestsBootErase() {
  auto& fs = populate();
  commandContext.origin = ORIGIN_SERIAL;
  check(std::strcmp(cmd_factoryreset(String("all")), "prompt") == 0);
  const size_t start = fs.log.size();
  const char* reply = factoryreset_confirmed(capturedUserData);
  check(startsWith(reply, "Factory reset complete"));
  check(std::strstr(reply, "NVS") != nullptr && std::strstr(reply, "resumes if power is lost") != nullptr);
  check(std::strlen(reply) < 319);  // not cut by the response buffer
  check(wipeArms == 1 && wipeDisarms == 0 && wipeArmed && wipeArmAt == start);
  check(!fs.exists(USERS_JSON_FILE) && committed() && rebootIntents == 1);
  for (const char* path : kRemoved) check(!fs.exists(path));

  // With the card out the longer reply still fits.
  auto& again = populate();
  again.sdAvailable() = false;
  reply = factoryreset_confirmed(capturedUserData ? capturedUserData : &commandContext);
  check(std::strstr(reply, "SD card not mounted") != nullptr && std::strlen(reply) < 319 && wipeArmed);
}

// A full reset that stops before the accounts are gone leaves no request
// behind, and one that cannot store the request touches nothing.
static void fullWipeRequestDroppedOnFailure() {
  auto& fs = populate();
  size_t start = fs.log.size();
  wipeArmOk = false;
  const char* reply = factoryreset_confirmed(&commandContext);
  check(startsWith(reply, "Error: factory reset stopped; the full wipe could not be requested"));
  check(fs.log.size() == start && notCommitted() && wipeArms == 1 && wipeDisarms == 0 && !wipeArmed);
  check(gSettings.phonePipeSecret == "fixture-secret" && idRecords == 0);

  populate(); otaPending = true;
  reply = factoryreset_confirmed(&commandContext);
  check(startsWith(reply, "Error: factoryreset all refused") && wipeArms == 0 && notCommitted());

  populate(); micFinalizeOk = false;
  reply = factoryreset_confirmed(&commandContext);
  check(startsWith(reply, "Error: factory reset aborted") && wipeArms == 0 && notCommitted());

  auto& third = populate(); pipeFailure = 3;
  reply = factoryreset_confirmed(&commandContext);
  check(startsWith(reply, "Error:") && third.exists(USERS_JSON_FILE) && notCommitted());
  check(wipeArms == 1 && wipeDisarms == 1 && !wipeArmed);

  auto& fourth = populate();
  fourth.faults.remove.add(USERS_JSON_FILE);
  reply = factoryreset_confirmed(&commandContext);
  check(startsWith(reply, "Error: factory reset failed") && fourth.exists(USERS_JSON_FILE) && notCommitted());
  check(wipeArms == 1 && wipeDisarms == 1 && !wipeArmed);
}

static void pipeClearFailurePreservesAccounts() {
  for(int failure=1;failure<=5;++failure){
#if !ENABLE_BLUETOOTH
    if(failure==1||failure==2||failure==4)continue;
#endif
    auto& fs=populate();pipeFailure=failure;
    const size_t start=fs.log.size();const char* reply=factoryreset_confirmed(nullptr);
    check(startsWith(reply,"Error:")&&fs.exists(USERS_JSON_FILE)&&notCommitted());
    check(fs.log.size()==start&&overlayClears==0&&idRecords==0);
    if(failure<=3)check(gSettings.phonePipeSecret=="fixture-secret"&&gSettings.phonePipeSecretGenerated);
  }
}
// Another task's BLE role transition: the reset stops before the pipe stops,
// with the secret, the mark, the accounts and the reboot untouched.
static void roleBusyTouchesNothing() {
#if ENABLE_BLUETOOTH
  auto& fs=populate();roleBusy=true;
  const size_t start=fs.log.size();const char* reply=factoryreset_confirmed(nullptr);
  check(std::strcmp(reply,"Error: factory reset stopped; BLE role transition busy")==0);
  check(!keyCleared&&!enrollmentCleared&&!secretPersisted&&roleDepth==0);
  check(gSettings.phonePipeSecret=="fixture-secret"&&gSettings.phonePipeSecretGenerated);
  check(fs.log.size()==start&&fs.exists(USERS_JSON_FILE)&&notCommitted()&&overlayClears==0&&idRecords==0);
#endif
}
struct Case { const char* name; void (*run)(); };
static const Case kCases[] = {
  {"pipeClearFailurePreservesAccounts",pipeClearFailurePreservesAccounts},
  {"roleBusyTouchesNothing",roleBusyTouchesNothing},
  {"wipeRemovesAccountData", wipeRemovesAccountData},
  {"sdNotMountedKeepsSdFolders", sdNotMountedKeepsSdFolders},
  {"accountDataFailureStopsBeforeRoster", accountDataFailureStopsBeforeRoster},
  {"leftoverSettingsStillReboots", leftoverSettingsStillReboots},
  {"micDrainFailureTouchesNothing", micDrainFailureTouchesNothing},
  {"promptNamesWhatIsDeleted", promptNamesWhatIsDeleted},
  {"fullWipePromptAndGates", fullWipePromptAndGates},
  {"fullWipeRequestsBootErase", fullWipeRequestsBootErase},
  {"fullWipeRequestDroppedOnFailure", fullWipeRequestDroppedOnFailure},
};

int main(int argc, char** argv) {
  unsigned ran = 0;
  for (const Case& c : kCases) {
    if (argc > 1 && std::strcmp(argv[1], c.name) != 0) continue;
    c.run();
    ++ran;
  }
  if (ran == 0) { std::fprintf(stderr, "unknown case: %s\n", argv[1]); return 2; }
  std::printf("factory reset wipe: %u checks passed in %u case(s)\n", checks, ran);
  return 0;
}
