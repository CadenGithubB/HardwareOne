// Cases for the automations.json mutation owner, appended by
// test_automation_store_mutations.py to the scheduler harness
// (automation_scheduler_harness.cpp, whose main becomes
// schedulerHarnessCases): its fake store, clock, state lock and the production
// cache section (which holds mutateAutomationsJson and the writer) are reused.
// The runner inserts the production commands and mutations below.
#include <map>
#include <string>

#include "System_ArgParse.h"
#include "System_AutomationId.h"
#include "System_CommandReply.h"

// ---------------------------------------------------------------------------
// Host boundaries for the inserted production code
// ---------------------------------------------------------------------------
static void* ps_alloc(size_t size, AllocPref, const char*) {
  void* result = std::malloc(size);
  if (result) ++allocations;
  return result;
}
static long gRandomStep = 1;
static long random(long low, long high) {
  const long value = low + gRandomStep % (high - low);
  ++gRandomStep;
  return value;
}
#define RETURN_VALID_IF_VALIDATE_CSTR() do {} while (false)
static constexpr size_t GLOBAL_DEBUG_BUFFER_SIZE = 512;
static char debugBufferDouble[GLOBAL_DEBUG_BUFFER_SIZE];
static bool ensureDebugBuffer() { return true; }
static char* getDebugBuffer() { return debugBufferDouble; }

// Two tasks print in the interleaving case: the records share one mutex.
static std::mutex recordMutex;
static std::vector<std::string> broadcasts, logLines;
static void recordFormatted(std::vector<std::string>& into, const char* format, va_list args) {
  char line[512];
  std::vsnprintf(line, sizeof(line), format, args);
  std::lock_guard<std::mutex> lock(recordMutex);
  into.emplace_back(line);
}
static void broadcastPrintf(const char* format, ...) {
  va_list args; va_start(args, format); recordFormatted(broadcasts, format, args); va_end(args);
}
#define BROADCAST_PRINTF(...) broadcastPrintf(__VA_ARGS__)
static void logSystemEvent(const char*, const char* format, ...) {
  va_list args; va_start(args, format); recordFormatted(logLines, format, args); va_end(args);
}
enum : int { SYSEVT_AUTOMATION_DELETED = 5 };
static std::vector<std::string> deletedEvents;
static void systemEventPost(int kind, const char* subject) {
  CHECK(kind == SYSEVT_AUTOMATION_DELETED);
  std::lock_guard<std::mutex> lock(recordMutex);
  deletedEvents.emplace_back(subject ? subject : "");
}

// The calling identity.
static String gCaller = "admin";
static bool gCallerIsAdmin = true;
static String currentExecUser() { return gCaller; }
static bool currentExecIsAdmin() { return gCallerIsAdmin; }
static void runAs(const char* user, bool admin) { gCaller = user; gCallerIsAdmin = admin; }

// The role half of the command policy (System_Utils.cpp), reduced to what the
// save-time owner check asks: "adminthing" rows need an administrator and
// "superthing" rows a super admin. A remote:/@ wrapper and roles are not
// modelled beyond that.
static const std::map<std::string, std::string> kRoles = {
    {"alice", "user"}, {"admin", "admin"}, {"root", "superadmin"}, {"guest", "guest"}};
enum class CommandRoleRefusal : uint8_t;
static std::vector<std::string> roleChecks;  // "owner|step" per check
static CommandRoleRefusal commandRoleRefusalFor(const String& user, const String& line, String* rowName);

// key=value arguments, as the automation commands read them.
class CommandArgs {
 public:
  explicit CommandArgs(const String& raw) : raw_(raw.c_str()) {}
  String value(const char* key) const {
    const std::string needle = std::string(key) + "=";
    size_t at = raw_.find(needle);
    if (at == std::string::npos || (at != 0 && raw_[at - 1] != ' ')) return String();
    at += needle.size();
    const size_t end = raw_.find(' ', at);
    return String(raw_.substr(at, end == std::string::npos ? std::string::npos : end - at));
  }
 private:
  std::string raw_;
};

// INSERT_STORE_PRODUCTION

static CommandRoleRefusal commandRoleRefusalFor(const String& user, const String& line, String* rowName) {
  roleChecks.push_back(std::string(user.c_str()) + "|" + line.c_str());
  const auto found = kRoles.find(user.c_str());
  if (found == kRoles.end()) return CommandRoleRefusal::RoleUnavailable;
  const std::string role = found->second, text = line.c_str();
  const std::string verb = text.substr(0, text.find(' '));
  if (rowName) *rowName = String(verb.c_str());
  if (role == "guest") return CommandRoleRefusal::Guest;
  if (verb == "superthing" && role != "superadmin") return CommandRoleRefusal::SuperAdmin;
  if (verb == "adminthing" && role != "admin" && role != "superadmin") return CommandRoleRefusal::Admin;
  return CommandRoleRefusal::None;
}

// userdelete (System_User.cpp): the account delete is a double, the automation
// sweep behind it is the production one above.
static bool filesystemReady = true;
#define DEBUG_USERSF(...) do {} while (false)
static constexpr int kRoleRankNoGrant = -1;
static std::vector<std::string> deletedUsers;
static bool userMutationAllowed(const String&, int, String&) { return true; }
static bool deleteUserInternal(const String& username, String& errorOut) {
  if (std::string(username.c_str()) == "nobody") { errorOut = "User not found"; return false; }
  deletedUsers.emplace_back(username.c_str());
  return true;
}

// INSERT_USER_DELETE

// ---------------------------------------------------------------------------
// Fixtures
// ---------------------------------------------------------------------------
// Another task holding the filesystem lock until this object goes away.
struct ForeignFsHold {
  std::promise<void> taken, release;
  std::thread holder;
  ForeignFsHold() {
    holder = std::thread([this] {
      FsLockGuard guard("other.task");
      CHECK(guard.held);
      taken.set_value();
      release.get_future().wait();
    });
    taken.get_future().wait();
  }
  ~ForeignFsHold() {
    release.set_value();
    holder.join();
  }
};

static std::string entry(long id, const char* name, const char* createdBy, const char* enabled,
                         const char* extra = "") {
  std::string text = "{\n  \"id\": " + std::to_string(id) + ",\n  \"name\": \"" + name + "\"";
  if (createdBy) text += std::string(",\n  \"createdBy\": \"") + createdBy + "\"";
  if (enabled) text += std::string(",\n  \"enabled\": ") + enabled;
  text += ",\n  \"triggers\": [\n    {\n      \"type\": \"interval\",\n      \"intervalMs\": 3600000,\n"
          "      \"nextAt\": 5000\n    }\n  ],\n  \"commands\": [\"ledon\"]";
  return text + extra + "\n}";
}

// The layout `automation add` has always written (hand-built text).
static std::string storeOf(const std::vector<std::string>& entries) {
  std::string text = "{\n  \"version\": 2,\n  \"automations\": [";
  for (size_t i = 0; i < entries.size(); ++i) text += (i ? ",\n" : "\n") + entries[i];
  return text + "\n]\n}\n";
}

static void putStore(const std::vector<std::string>& entries) {
  reset(storeOf(entries).c_str());
  broadcasts.clear(); logLines.clear(); deletedEvents.clear(); deletedUsers.clear();
  runAs("admin", true);
}

static std::string stored() {
  std::lock_guard<std::mutex> lock(storeMutex);
  return storeJson.c_str();
}

// The stored automation with this id, parsed; `doc` keeps it alive.
static JsonObjectConst storedEntry(JsonDocument& doc, long id) {
  CHECK(!deserializeJson(doc, stored()));
  for (JsonObjectConst automation : doc["automations"].as<JsonArrayConst>()) {
    if (automation["id"].as<long>() == id) return automation;
  }
  return JsonObjectConst();
}
static bool enabledOf(long id) {
  JsonDocument doc;
  JsonObjectConst automation = storedEntry(doc, id);
  CHECK(!automation.isNull());
  return automation["enabled"] | false;
}
static size_t storedCount() {
  JsonDocument doc;
  CHECK(!deserializeJson(doc, stored()));
  return doc["automations"].as<JsonArrayConst>().size();
}

// What the text readers rely on (runAutomationsOnBoot, the scheduler pass and
// `automation run`): `"id": <n>` with one space, the id found as a whole
// number, and "id" the first member of its object.
static void checkReadersFind(long id) {
  const std::string text = stored();
  const int at = automationIdPosInJson(String(text), static_cast<unsigned long>(id));
  CHECK(at >= 0);
  const size_t open = text.rfind('{', static_cast<size_t>(at));
  CHECK(open != std::string::npos);
  for (size_t i = open + 1; i < static_cast<size_t>(at); ++i) {
    CHECK(text[i] == ' ' || text[i] == '\n' || text[i] == '\r' || text[i] == '\t');
  }
}

static bool is(const char* reply, const char* expected) { return reply && std::strcmp(reply, expected) == 0; }

// ---------------------------------------------------------------------------
// Cases
// ---------------------------------------------------------------------------
static void changesGoByNumericId() {
  putStore({entry(123456, "long", "alice", "true"), entry(12, "short", "alice", "true"),
            entry(1234, "middle", "bob", "true")});
  CHECK(is(cmd_automation_enable_disable("id=12", false), "OK"));
  CHECK(!enabledOf(12) && enabledOf(123456) && enabledOf(1234));
  CHECK(writes == 1 && broadcasts.back() == "Disabled automation id=12");
  CHECK(is(cmd_automation_enable_disable("id=1234", false), "OK"));
  CHECK(!enabledOf(12) && enabledOf(123456) && !enabledOf(1234));
  CHECK(is(cmd_automation_enable_disable("id=12", true), "OK"));
  CHECK(enabledOf(12) && enabledOf(123456) && !enabledOf(1234));
  for (long id : {123456L, 12L, 1234L}) checkReadersFind(id);
  // The state it already has: still OK, and the file is not rewritten.
  const unsigned before = writes;
  CHECK(is(cmd_automation_enable_disable("id=12", true), "OK") && writes == before);

  // An id that is only part of a stored one, or no id at all.
  for (const char* partial : {"id=1", "id=123", "id=12345", "id=1234567"}) {
    CHECK(is(cmd_automation_enable_disable(partial, false), "Error: automation id not found"));
    CHECK(is(cmd_automation_delete(partial), "Error: automation id not found"));
  }
  CHECK(is(cmd_automation_enable_disable("id=12x", false), kAutomationIdArgReply));
  CHECK(is(cmd_automation_delete("id=0"), kAutomationIdArgReply));
  CHECK(writes == before && storedCount() == 3);

  // Delete the short id: the other two stay, whole.
  CHECK(is(cmd_automation_delete("id=12"), "OK"));
  CHECK(storedCount() == 2 && enabledOf(123456) && !enabledOf(1234));
  CHECK(deletedEvents.size() == 1 && deletedEvents[0] == "short");
  CHECK(broadcasts.back() == "Deleted automation id=12");
  checkReadersFind(123456);
  checkReadersFind(1234);
  CHECK(is(cmd_automation_delete("id=1234"), "OK") && is(cmd_automation_delete("id=123456"), "OK"));
  CHECK(storedCount() == 0);
  JsonDocument doc;
  CHECK(!deserializeJson(doc, stored()) && doc["version"] == 2);

  // Only the creator or an admin.
  putStore({entry(12, "mine", "alice", "true"), entry(1234, "theirs", "bob", "true")});
  runAs("alice", false);
  CHECK(std::strstr(cmd_automation_enable_disable("id=1234", false), "do not have permission to modify"));
  CHECK(std::strstr(cmd_automation_delete("id=1234"), "do not have permission to delete"));
  CHECK(writes == 0 && enabledOf(1234));
  CHECK(is(cmd_automation_enable_disable("id=12", false), "OK") && !enabledOf(12));
  CHECK(is(cmd_automation_delete("id=12"), "OK") && storedCount() == 1);
}

static void quotesAndBracesSurvive() {
  // A name, a condition and commands holding quotes, braces and brackets; an
  // entry without an "enabled" flag sits before one that has it.
  const char* tricky =
      "{\n  \"id\": 12,\n  \"name\": \"say \\\"hi\\\" {now} [x], \\\"enabled\\\": false\",\n"
      "  \"createdBy\": \"alice\",\n  \"enabled\": true,\n  \"condition\": \"IF TEMP>20 THEN {a} ELSE \\\"b\\\"\",\n"
      "  \"triggers\": [\n    {\n      \"type\": \"boot\",\n      \"nextAt\": null\n    }\n  ],\n"
      "  \"commands\": [\"echo \\\"a}b\\\"\", \"x{y\", \"note \\\"id\\\": 1234, ]\"]\n}";
  const char* flagless =
      "{\n  \"id\": 77,\n  \"name\": \"no flag\",\n  \"createdBy\": \"alice\",\n"
      "  \"triggers\": [],\n  \"commands\": [\"ledoff\"]\n}";
  putStore({tricky, flagless, entry(1234, "after", "bob", "true")});
  std::string name, condition, first, third;
  {
    JsonDocument doc;
    JsonObjectConst before = storedEntry(doc, 12);
    name = before["name"] | "";
    condition = before["condition"] | "";
    first = before["commands"][0] | "";
    third = before["commands"][2] | "";
    CHECK(name == "say \"hi\" {now} [x], \"enabled\": false" && first == "echo \"a}b\"");
  }
  for (bool enable : {false, true, false}) {
    CHECK(is(cmd_automation_enable_disable("id=12", enable), "OK"));
    JsonDocument doc;
    JsonObjectConst after = storedEntry(doc, 12);
    CHECK((after["enabled"] | !enable) == enable);
    CHECK(name == (after["name"] | "") && condition == (after["condition"] | ""));
    CHECK(after["commands"].as<JsonArrayConst>().size() == 3);
    CHECK(first == (after["commands"][0] | "") && third == (after["commands"][2] | ""));
    CHECK(std::string(after["commands"][1] | "") == "x{y" && after["triggers"][0]["nextAt"].isNull());
    CHECK(enabledOf(1234) && storedCount() == 3);
  }
  // The id inside the command text is not an automation.
  CHECK(is(cmd_automation_enable_disable("id=1234", false), "OK") && !enabledOf(1234));
  CHECK(is(cmd_automation_enable_disable("id=1234", true), "OK") && enabledOf(1234));
  // An entry without the flag is refused; the next automation's flag is not
  // taken for it.
  const unsigned before = writes;
  CHECK(is(cmd_automation_enable_disable("id=77", false), "Error: malformed automation"));
  CHECK(writes == before && enabledOf(1234));
  for (long id : {12L, 77L, 1234L}) checkReadersFind(id);
  // Deleting the tricky one leaves the others whole.
  CHECK(is(cmd_automation_delete("id=12"), "OK"));
  CHECK(deletedEvents.back() == name && storedCount() == 2 && enabledOf(1234));
}

static AutomationsMutateResult add(const char* entryJson, bool hasId, long& id, uint8_t extraFlags = 0,
                                   const char** replyOut = nullptr, const char* stepsText = "echo x") {
  JsonDocument built;
  CHECK(!deserializeJson(built, entryJson));
  const String steps(stepsText);
  AutomationAddContext ctx{built.as<JsonObjectConst>(), hasId, id, currentExecUser(), currentExecIsAdmin(),
                           nullptr, &steps};
  String error;
  const AutomationsMutateResult result = mutateAutomationsJson(
      "automation.add", automationAddMutation, &ctx,
      static_cast<uint8_t>(kAutomationsMutateNotify | kAutomationsMutateSeed | extraFlags), error);
  id = ctx.id;
  if (replyOut) *replyOut = ctx.reply;
  return result;
}

static void addIsOneWriteAndIdStaysFirst() {
  const char* built = R"({"id":0,"name":"new \"one\"","createdBy":"alice","enabled":true,)"
                      R"("triggers":[{"type":"boot","nextAt":null}],"commands":["echo {x}"]})";
  // No file yet: a dry run writes nothing, not even the empty store.
  reset(nullptr);
  runAs("alice", false);
  long id = 0;
  CHECK(add(built, false, id, kAutomationsMutateDryRun) == AutomationsMutateResult::Changed);
  CHECK(writes == 0 && !storeExists);
  // The first add: one write, the current format, the automation in it.
  clockMs = 5000;
  id = 0;
  CHECK(add(built, false, id) == AutomationsMutateResult::Changed);
  CHECK(writes == 1 && storeExists && id >= 1 && id <= static_cast<long>(kAutomationIdMax));
  {
    JsonDocument doc;
    JsonObjectConst automation = storedEntry(doc, id);
    CHECK(doc["version"] == 2 && storedCount() == 1 && !automation.isNull());
    CHECK(std::string(automation["name"] | "") == "new \"one\"" && automation["triggers"][0]["nextAt"].isNull());
    CHECK(std::string(automation["commands"][0] | "") == "echo {x}");
  }
  checkReadersFind(id);
  CHECK(stored().find("\"enabled\": true") != std::string::npos);
  CHECK(stored().find("\"type\": \"boot\"") != std::string::npos);
  // A second add at the same uptime gets another id.
  long second = 0;
  CHECK(add(built, false, second) == AutomationsMutateResult::Changed && second != id && storedCount() == 2);
  checkReadersFind(second);

  // An edit through id=: only the creator or an admin, and the entry is replaced.
  const char* edited = R"({"id":0,"name":"edited","createdBy":"bob","enabled":false,"triggers":[],"commands":[]})";
  runAs("bob", false);
  const char* reply = nullptr;
  long target = id;
  const unsigned before = writes;
  CHECK(add(edited, true, target, 0, &reply) == AutomationsMutateResult::Refused);
  CHECK(reply && std::strstr(reply, "do not have permission to edit") && writes == before);
  runAs("alice", false);
  CHECK(add(edited, true, target) == AutomationsMutateResult::Changed && target == id);
  {
    JsonDocument doc;
    JsonObjectConst automation = storedEntry(doc, id);
    CHECK(storedCount() == 2 && std::string(automation["name"] | "") == "edited" && !(automation["enabled"] | true));
  }
  checkReadersFind(id);
  checkReadersFind(second);
  // id= that names no stored automation adds one under that id.
  long fresh = 4242;
  CHECK(add(edited, true, fresh) == AutomationsMutateResult::Changed && fresh == 4242 && storedCount() == 3);
  checkReadersFind(4242);
}

// B2 (owner decision 2026-11-21): every step, THEN/ELSE branches included, is
// checked against the OWNER's role when an automation is saved, and an edit
// never changes the owner, also when an administrator edits it.
static void ownerRoleCheckedAtSaveAndKeptOnEdit() {
  const char* built = R"({"id":0,"name":"steps","createdBy":"x","enabled":true,)"
                      R"("triggers":[{"type":"boot","nextAt":null}],"commands":["echo"]})";
  reset(nullptr);
  const char* reply = nullptr;
  long id = 0;
  // An administrator may store an admin step, not a super-admin one, and not
  // one hidden in a branch.
  runAs("admin", true);
  CHECK(add(built, false, id, 0, nullptr, "echo a;adminthing on") == AutomationsMutateResult::Changed);
  JsonDocument doc;
  CHECK(std::string(storedEntry(doc, id)["createdBy"] | "") == "admin");
  for (const char* steps : {"superthing now", "echo;superthing", "IF TEMP>30 THEN echo hot ELSE superthing",
                            "IF A=1 THEN echo ELSE IF B=2 THEN superthing", "PRINT hi;superthing"}) {
    roleChecks.clear();
    long fresh = 0;
    const unsigned before = writes;
    CHECK(add(built, false, fresh, 0, &reply, steps) == AutomationsMutateResult::Refused);
    CHECK(reply && std::strstr(reply, "needs a super admin") && std::strstr(reply, "'admin'") && writes == before);
    for (const std::string& check : roleChecks) CHECK(check.rfind("admin|", 0) == 0);
  }
  // PRINT is not a command; the branch text is checked, not the condition.
  roleChecks.clear();
  long printed = 0;
  CHECK(add(built, false, printed, 0, nullptr, "PRINT superthing;IF TEMP>30 THEN echo") ==
        AutomationsMutateResult::Changed);
  CHECK(roleChecks.size() == 1 && roleChecks[0] == "admin|echo");

  // A super admin edits the administrator's automation: the owner stays, and
  // the steps are checked against the owner, not the editor.
  runAs("root", true);
  long target = id;
  CHECK(add(built, true, target, 0, &reply, "superthing") == AutomationsMutateResult::Refused);
  CHECK(reply && std::strstr(reply, "'admin'"));
  CHECK(add(built, true, target, 0, nullptr, "adminthing off") == AutomationsMutateResult::Changed);
  CHECK(target == id && std::string(storedEntry(doc, id)["createdBy"] | "") == "admin");
  // The super admin's own new automation may hold the super step.
  long own = 0;
  CHECK(add(built, false, own, 0, nullptr, "superthing") == AutomationsMutateResult::Changed);
  CHECK(std::string(storedEntry(doc, own)["createdBy"] | "") == "root");
  // An administrator editing the super admin's automation cannot add a step
  // only a super admin may run (it would run as the super admin), and the
  // check uses the editor's name; an edit within the editor's own rights
  // keeps the super admin as owner.
  runAs("admin", true);
  long theirs = own;
  CHECK(add(built, true, theirs, 0, &reply, "superthing again") == AutomationsMutateResult::Refused);
  CHECK(reply && std::strstr(reply, "needs a super admin") && std::strstr(reply, "'admin' is not"));
  CHECK(add(built, true, theirs, 0, nullptr, "adminthing") == AutomationsMutateResult::Changed);
  CHECK(std::string(storedEntry(doc, own)["createdBy"] | "") == "root");
  // The save splits a conditional the way the run does: a THEN or ELSE inside
  // quotes still splits it, so the step behind it is checked.
  long quoted = 0;
  CHECK(add(built, false, quoted, 0, &reply, "IF A=1 THEN echo \"x ELSE superthing now\"") ==
        AutomationsMutateResult::Refused);
  runAs("root", true);
  // An owner whose role cannot be read: nothing is stored.
  runAs("ghost", true);
  long lost = 0;
  CHECK(add(built, false, lost, 0, &reply, "echo") == AutomationsMutateResult::Refused);
  CHECK(reply && std::strstr(reply, "could not be read"));
  runAs("admin", true);
}

static void interleavedMutationsBothLand() {
  putStore({entry(12, "first", "alice", "true"), entry(1234, "second", "bob", "true"),
            entry(99, "third", "bob", "true")});
  // While the first mutation is between its read and its write a second one
  // starts on another task. It must wait, not write from its own stale read.
  std::atomic<bool> secondDone{false};
  std::atomic<int> disabledForBob{-2};
  std::thread second;
  bool started = false;
  afterRead = [&] {
    if (started) return;
    started = true;
    second = std::thread([&] {
      disabledForBob = automationDisableOwnedBy("bob");
      secondDone = true;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    CHECK(!secondDone);  // held out by the lock the first mutation still has
  };
  CHECK(is(cmd_automation_enable_disable("id=12", false), "OK"));
  second.join();
  afterRead = {};
  CHECK(secondDone && disabledForBob == 2);
  CHECK(!enabledOf(12) && !enabledOf(1234) && !enabledOf(99) && storedCount() == 3);
  CHECK(writes == 2);

  // The same with a delete racing a toggle.
  putStore({entry(12, "first", "alice", "true"), entry(1234, "second", "bob", "true")});
  started = false;
  std::atomic<bool> deleted{false};
  afterRead = [&] {
    if (started) return;
    started = true;
    second = std::thread([&] { deleted = is(cmd_automation_delete("id=1234"), "OK"); });
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    CHECK(!deleted);
  };
  CHECK(is(cmd_automation_enable_disable("id=12", false), "OK"));
  second.join();
  afterRead = {};
  CHECK(deleted && storedCount() == 1 && !enabledOf(12));
}

static void brokenDocumentsAreNotPublished() {
  putStore({entry(12, "kept", "alice", "true")});
  const std::string good = stored();
  // The public writer refuses what no reader could load.
  for (const char* bad : {"{broken", "", "[]", R"({"version":2})", R"({"version":2,"automations":{}})",
                          R"({"version":3,"automations":[]})"}) {
    CHECK(!writeAutomationsJsonAtomic(bad));
  }
  CHECK(stored() == good && writes == 0);
  // A failed write leaves the file and reports it.
  writeSucceeds = false;
  CHECK(is(cmd_automation_enable_disable("id=12", false), "Error: failed to write automations.json"));
  CHECK(is(cmd_automation_delete("id=12"), "Error: failed to write automations.json"));
  writeSucceeds = true;
  CHECK(stored() == good);
  // A stored file that does not parse is left as it is by every mutation.
  for (const char* damaged : {"{\"version\": 2, \"automations\": [ {\"id\": 12, ", "{\"version\":2}"}) {
    reset(damaged);
    CHECK(is(cmd_automation_enable_disable("id=12", false), "Error: malformed JSON"));
    CHECK(is(cmd_automation_delete("id=12"), "Error: malformed JSON"));
    long id = 0;
    CHECK(add(R"({"id":0,"name":"n"})", false, id) == AutomationsMutateResult::Malformed);
    CHECK(automationDisableOwnedBy("alice") == -1);
    CHECK(writes == 0 && stored() == damaged);
  }
  // A file that is there and cannot be read is never taken for a missing one.
  putStore({entry(12, "kept", "alice", "true")});
  readSucceeds = false;
  CHECK(std::strstr(cmd_automation_enable_disable("id=12", false), "failed to read automations.json"));
  long id = 0;
  CHECK(add(R"({"id":0,"name":"n"})", false, id) == AutomationsMutateResult::ReadFailed);
  CHECK(automationDisableOwnedBy("alice") == -1);
  readSucceeds = true;
  CHECK(writes == 0 && stored() == good);
  // A newer file is refused by name and left alone.
  reset(R"({"version":3,"automations":[{"id":12,"enabled":true}]})");
  CHECK(std::strstr(cmd_automation_enable_disable("id=12", false), "newer than this firmware reads"));
  CHECK(automationDisableOwnedBy("alice") == -1 && writes == 0);
}

static void sanitizeStoreIsOneStep() {
  putStore({entry(7, "a", "alice", "true"), entry(7, "b", "alice", "true"), entry(8, "c", "bob", "true")});
  AutomationSanitizeCounts counts;
  bool changed = false;
  CHECK(automationsSanitizeStore(&counts, &changed) && changed && counts.duplicates == 1 && counts.outOfRange == 0);
  CHECK(writes == 1 && storedCount() == 3);
  changed = true;
  CHECK(automationsSanitizeStore(&counts, &changed) && !changed && counts.duplicates == 0 && writes == 1);
  CHECK(automationsSanitizeStore());
  reset(nullptr);
  changed = true;
  CHECK(!automationsSanitizeStore(&counts, &changed) && !changed && writes == 0 && !storeExists);
}

// The next-run writebacks of the commands go by numeric id too, and a
// recompute with nothing to rewrite writes nothing.
static void nextRunWritebacks() {
  putStore({entry(123456, "long", "alice", "true"), entry(12, "short", "alice", "true"),
            entry(1234, "off", "bob", "false")});
  CHECK(updateAutomationTriggerNextAt(12, 0, 777) && writes == 1);
  auto nextAt = [](long id) {
    JsonDocument doc;
    return storedEntry(doc, id)["triggers"][0]["nextAt"].as<unsigned long>();
  };
  CHECK(nextAt(12) == 777 && nextAt(123456) == 5000 && nextAt(1234) == 5000);
  CHECK(!updateAutomationTriggerNextAt(1, 0, 888) && !updateAutomationTriggerNextAt(12, 3, 888) && writes == 1);
  for (long id : {123456L, 12L, 1234L}) checkReadersFind(id);
  // Recompute: every clock trigger of the enabled automations, in one write.
  int skipped = -1;
  CHECK(automationRecomputeAllClockTriggers(100000, &skipped) == 2 && skipped == 1 && writes == 2);
  CHECK(nextAt(12) == 103600 && nextAt(123456) == 103600 && nextAt(1234) == 5000);
  // Nothing enabled: nothing to rewrite, nothing written.
  putStore({entry(1234, "off", "bob", "false")});
  CHECK(automationRecomputeAllClockTriggers(100000, &skipped) == 0 && skipped == 1 && writes == 0);
  reset(nullptr);
  CHECK(automationRecomputeAllClockTriggers(100000, &skipped) == -1 && writes == 0 && !storeExists);
}

// UA-7: deleting a user disables the automations that user created.
static void deletedUsersAutomationsAreDisabled() {
  const std::vector<std::string> rules = {
      entry(12, "alice one", "alice", "true"), entry(1234, "alice two", "alice", "true"),
      entry(20, "alice off", "alice", "false"), entry(30, "bob's", "bob", "true"),
      entry(40, "other case", "Alice", "true"), entry(50, "nobody's", nullptr, "true"),
      entry(60, "prefix", "alice2", "true")};
  putStore(rules);
  CHECK(startAutomationScheduler());
  automationTick();
  const unsigned passes = ticks;
  CHECK(is(doUserDelete("alice"), "Deleted user 'alice'; 2 automations disabled"));
  CHECK(deletedUsers.size() == 1 && deletedUsers[0] == "alice");
  CHECK(!enabledOf(12) && !enabledOf(1234) && !enabledOf(20));
  CHECK(enabledOf(30) && enabledOf(40) && enabledOf(50) && enabledOf(60));
  CHECK(writes == 1 && storedCount() == rules.size());
  CHECK(logLines.size() == 1 && logLines[0] == "2 automation(s) of removed user 'alice' disabled");
  // They stay in the file under their creator, disabled: adding the name
  // again re-enables nothing.
  {
    JsonDocument doc;
    CHECK(std::string(storedEntry(doc, 12)["createdBy"] | "") == "alice");
  }
  for (long id : {12L, 1234L, 30L}) checkReadersFind(id);
  // The scheduler is told: its next lap runs a pass on the changed file.
  automationTick();
  CHECK(ticks == passes + 1);
  stopAutomationScheduler();

  // One automation, and none.
  CHECK(is(doUserDelete("bob"), "Deleted user 'bob'; 1 automation disabled") && !enabledOf(30));
  const unsigned before = writes;
  CHECK(is(doUserDelete("carol"), "Deleted user 'carol'") && writes == before);
  CHECK(is(doUserDelete("alice"), "Deleted user 'alice'") && writes == before);  // nothing left to disable
  // A delete that failed touches no automation.
  CHECK(is(doUserDelete("nobody"), "Error: User not found") && writes == before && enabledOf(40));
  CHECK(automationDisableOwnedBy("") == 0 && writes == before && enabledOf(50));

  // A store that cannot be used does not undo or block the delete.
  reset(R"({"version":3,"automations":[{"id":12,"createdBy":"dave","enabled":true}]})");
  deletedUsers.clear();
  const char* reply = doUserDelete("dave");
  CHECK(std::strncmp(reply, "Deleted user 'dave'; their automations were not checked", 55) == 0);
  CHECK(deletedUsers.size() == 1 && writes == 0);
  // No automations file at all is simply no automations.
  reset(nullptr);
  CHECK(is(doUserDelete("erin"), "Deleted user 'erin'") && writes == 0 && !storeExists);
}

// The scheduler claims a pass only after a filesystem try-lock.
static void passWaitsForTheFilesystemLock() {
  reset(eventRules);
  CHECK(startAutomationScheduler());
  automationTick();
  CHECK(ticks == 1 && gAutomationsCacheValid);
  const unsigned readsBefore = reads;

  // A subscribed event arrives and a save is announced while another task
  // holds the lock: the tick returns at once and consumes neither.
  clockMs += 300;
  const uint32_t seq = postEvent(1);
  automationOnSystemEvent(1);
  notifyAutomationScheduler();
  {
    ForeignFsHold other;
    const unsigned missesBefore = fsTryMisses;
    const auto started = std::chrono::steady_clock::now();
    for (int lap = 0; lap < 5; ++lap) automationTick();
    CHECK(std::chrono::steady_clock::now() - started < std::chrono::milliseconds(500));
    CHECK(fsTryMisses == missesBefore + 5);
    CHECK(ticks == 1 && reads == readsBefore && writes == 0);
    // Not a missing file: the cache is still the valid one it had, the refresh
    // and the event are still pending.
    CHECK(gAutomationsCacheValid && gAutomationsCacheCount == 1);
    CHECK(sAutomationRefreshPending && gAutomationEventsPending);
  }
  // The first lap after the lock is free runs the pass and sees the event.
  automationTick();
  CHECK(ticks == 2 && drainedSeqs.size() == 1 && drainedSeqs[0] == seq);
  CHECK(!sAutomationRefreshPending && gAutomationsCacheValid);

  // The task's own hold is not "busy" (a tick reached under a guard), and a
  // device without the lock yet still ticks.
  {
    FsLockGuard own("outer");
    CHECK(own.held);
    notifyAutomationScheduler();
    automationTick();
    CHECK(ticks == 3);
  }
  void* const mutex = gFsMutex;
  gFsMutex = nullptr;
  notifyAutomationScheduler();
  automationTick();
  gFsMutex = mutex;
  CHECK(ticks == 4);

  // With nothing to do the lock is not even tried.
  {
    ForeignFsHold other;
    const unsigned missesBefore = fsTryMisses;
    for (int lap = 0; lap < 5; ++lap) automationTick();
    CHECK(fsTryMisses == missesBefore && ticks == 4);
  }
  stopAutomationScheduler();
}

int main() {
  schedulerHarnessCases();
  const unsigned inherited = checks.load();
  changesGoByNumericId();
  quotesAndBracesSurvive();
  addIsOneWriteAndIdStaysFirst();
  ownerRoleCheckedAtSaveAndKeptOnEdit();
  interleavedMutationsBothLand();
  brokenDocumentsAreNotPublished();
  sanitizeStoreIsOneStep();
  nextRunWritebacks();
  deletedUsersAutomationsAreDisabled();
  passWaitsForTheFilesystemLock();
  reset();
  std::printf("automation store mutations: %u checks passed (after %u scheduler harness checks)\n",
              checks.load() - inherited, inherited);
}
