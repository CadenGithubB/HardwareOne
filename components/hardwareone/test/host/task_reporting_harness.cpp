#include <Arduino.h>
#include <ArduinoJson.h>
#include <algorithm>
#include <cassert>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <map>
#include <string>
#include <vector>

void webBatchTestStringAppend() {}
using UBaseType_t = uint32_t;
enum eTaskState { eRunning, eReady, eBlocked, eSuspended, eDeleted, eInvalid };
struct TaskStatus_t {
  uintptr_t xHandle;
  const char* pcTaskName;
  eTaskState eCurrentState;
  UBaseType_t uxCurrentPriority;
  uint32_t usStackHighWaterMark;
  uint32_t ulRunTimeCounter;
};
struct Snapshot { std::vector<TaskStatus_t> rows; uint32_t total; };
struct Allocation { size_t bytes; std::string tag; };
enum class AllocPref { PreferPSRAM };
static std::map<void*, Allocation> live;
static std::vector<std::string> output, events;
static std::vector<Snapshot> snapshots;
static UBaseType_t reportedCount;
static unsigned allocCalls, freeCalls, countCalls, snapshotCalls, delayCalls, loopReads, loopPrints;
static unsigned failAllocation;
static bool validateOnly, loopValid = true, cpuCommand, jsonCommand;
static uint32_t laps = 123, avg = 8, maximum = 37, stalls = 2, allStalls = 19;
static char debugBuffer[4096];
static constexpr size_t GLOBAL_DEBUG_BUFFER_SIZE = sizeof(debugBuffer);
static unsigned checks;
static void checkAt(bool good, int line) {
  ++checks;
  if (!good) std::fprintf(stderr, "task-reporting assertion line %d\n", line);
  assert(good);
}
#define CHECK(value) checkAt((value), __LINE__)
#define RETURN_VALID_IF_VALIDATE_CSTR() do { if (validateOnly) return "VALID"; } while (0)
#define PSRAM_JSON_DOC(name) JsonDocument name
static bool argWantsJson(const String& value) { return value.indexOf("json") >= 0; }
static char* getDebugBuffer() { return debugBuffer; }
static void* ps_alloc(size_t bytes, AllocPref pref, const char* tag) {
  CHECK(pref == AllocPref::PreferPSRAM && bytes > 0);
  ++allocCalls; events.emplace_back("allocate");
  const char* expectedTag = !cpuCommand ? "taskstats" :
      jsonCommand ? (allocCalls == 1 ? "perftop.ja" : "perftop.jb") :
                    (allocCalls == 1 ? "perftop.a" : "perftop.b");
  CHECK(std::strcmp(tag, expectedTag) == 0);
  if (allocCalls == failAllocation) return nullptr;
  void* memory = std::calloc(1, bytes);
  CHECK(memory != nullptr); live.emplace(memory, Allocation{bytes, tag}); return memory;
}
static void snapshotFree(void* memory) {
  CHECK(memory && live.count(memory) == 1);
  ++freeCalls; events.emplace_back("free");
  std::memset(memory, 0xa5, live.at(memory).bytes);
  live.erase(memory); std::free(memory);
}
static UBaseType_t uxTaskGetNumberOfTasks() {
  ++countCalls; events.emplace_back("count"); return reportedCount;
}
static UBaseType_t uxTaskGetSystemState(TaskStatus_t* destination, UBaseType_t capacity, uint32_t* total) {
  CHECK(snapshotCalls < snapshots.size());
  CHECK(live.count(destination) == 1 && live.at(destination).bytes >= capacity * sizeof(TaskStatus_t));
  if (cpuCommand) CHECK(live.at(destination).bytes == capacity * sizeof(TaskStatus_t));
  CHECK(capacity == reportedCount + (cpuCommand ? 6 : 0));
  CHECK(cpuCommand == (total != nullptr));
  events.emplace_back("snapshot");
  const auto& snapshot = snapshots[snapshotCalls++];
  CHECK(snapshot.rows.size() <= capacity);
  std::copy(snapshot.rows.begin(), snapshot.rows.end(), destination);
  if (total) *total = snapshot.total;
  return static_cast<UBaseType_t>(snapshot.rows.size());
}
static uint32_t pdMS_TO_TICKS(uint32_t milliseconds) { return milliseconds; }
static void vTaskDelay(uint32_t ticks) {
  CHECK(ticks == 750 && snapshotCalls == 1 && live.size() == 2);
  ++delayCalls; events.emplace_back("delay750");
}
static void broadcastOutput(const char* text) {
  if (cpuCommand && (!std::strcmp(text, "========== END PERFTOP ==========") ||
                     !std::strcmp(text, "  (cannot allocate task snapshot buffers)"))) CHECK(live.empty());
  CHECK(std::strlen(text) < 256); output.emplace_back(text); events.emplace_back("broadcast");
}
static void broadcastPrintf(const char* format, ...) {
  char buffer[256]; va_list args; va_start(args, format);
  const int length = std::vsnprintf(buffer, sizeof(buffer), format, args); va_end(args);
  CHECK(length >= 0 && length < static_cast<int>(sizeof(buffer))); broadcastOutput(buffer);
}
#define BROADCAST_PRINTF(...) broadcastPrintf(__VA_ARGS__)
static bool perfGetLoopSnapshot(uint32_t& l, uint32_t& a, uint32_t& m, uint32_t& s, uint32_t& t) {
  ++loopReads; events.emplace_back("loop.read"); l = laps; a = avg; m = maximum; s = stalls; t = allStalls; return loopValid;
}
static void perfPrintLoopHealth() {
  ++loopPrints; events.emplace_back("loop.print"); broadcastOutput("fixture loop health");
}
static uint32_t taskStackLookup(const char* name) {
  return name && !std::strcmp(name, "worker") ? 4096 : 0;
}
static size_t checkedSerializeJson(const JsonDocument& doc, char* destination, size_t capacity) {
  // Serialization must run before snapshot buffers are released. The buffers
  // do not own FreeRTOS task names; this assertion checks only their lifetime.
  CHECK(live.size() == (cpuCommand ? (failAllocation ? 1u : 2u) : 1u));
  events.emplace_back("serialize");
  return ArduinoJson::serializeJson(doc, destination, capacity);
}
#define serializeJson checkedSerializeJson
#define free snapshotFree
// INSERT_PRODUCTION
#undef free
#undef serializeJson

static TaskStatus_t task(uintptr_t handle, const char* name, uint32_t runtime,
                         eTaskState state = eReady, uint32_t priority = 4, uint32_t stack = 7) {
  return {handle, name, state, priority, stack, runtime};
}
static void resetCommand() {
  output.clear(); events.clear(); snapshots.clear(); debugBuffer[0] = '\0';
  allocCalls = freeCalls = countCalls = snapshotCalls = delayCalls = loopReads = loopPrints = 0;
  failAllocation = 0; validateOnly = jsonCommand = false; loopValid = true; cpuCommand = true;
  laps = 123; avg = 8; maximum = 37; stalls = 2; allStalls = 19;
}
static JsonDocument parse(const char* result) {
  JsonDocument doc; CHECK(!deserializeJson(doc, result)); return doc;
}
static void transcript(const char* command, const char* result) {
  // Print all wire-visible data, including per-line boundaries, for optional
  // baseline parity. Allocation tags/counters are checked separately.
  JsonDocument record; record["command"] = command; record["result"] = result;
  JsonArray messages = record["broadcast"].to<JsonArray>();
  for (const auto& line : output) messages.add(line);
  std::string text; serializeJson(record, text); std::cout << text << '\n';
}
static void checkLifetimeAndTiming(bool json, bool allocationFailure = false) {
  CHECK(countCalls == 1 && allocCalls == 2 && live.empty());
  CHECK(freeCalls == (allocationFailure ? 1u : 2u));
  CHECK(snapshotCalls == (allocationFailure ? 0u : 2u));
  CHECK(delayCalls == (allocationFailure ? 0u : 1u));
  CHECK(loopReads == (json ? 1u : 0u) && loopPrints == (json ? 0u : 1u));
  const auto count = std::find(events.begin(), events.end(), "count");
  CHECK(std::find(events.begin(), events.end(), json ? "loop.read" : "loop.print") < count);
  if (json) CHECK(std::find(events.begin(), events.end(), "serialize") < std::find(events.begin(), events.end(), "free"));
}
static void cpuFixture(const std::string& scenario, std::vector<std::string>& names,
                       std::vector<unsigned>& percentages, std::vector<bool>& idle, bool& available) {
  available = true;
  if (scenario == "normal") {
    snapshots = {{{task(1,"worker",100),task(2,"IDLE0",10),task(3,"IDLE1",200),task(4,"tiny",50),
                   task(5,"long-worker-name-EXTRA",0),task(6,nullptr,0),task(7,"reused",1000),task(8,"gone",0),
                   task(10,"IDLEworker",20),task(11,"idle0",0)},1000},
                 {{task(1,"worker",350),task(12,"new",200),task(3,"IDLE1",700),task(4,"tiny",59),
                   task(7,"reused",1),task(6,nullptr,100),task(5,"long-worker-name-EXTRA",10),task(2,"IDLE0",10),
                   task(11,"idle0",0),task(10,"IDLEworker",20)},2000}};
    names = {"worker","IDLE1","long-worker-name-EXTRA","IDLE0","IDLEworker"};
    percentages = {25,50,1,0,0}; idle = {false,true,false,true,true};
  } else if (scenario == "rollover") {
    snapshots = {{{task(1,"worker",UINT32_MAX-49),task(2,"reused",100)},UINT32_MAX-99},
                 {{task(2,"reused",90),task(1,"worker",50)},100}};
    names={"worker"}; percentages={50}; idle={false};
  } else if (scenario == "cutoff") {
    snapshots = {{{task(1,"below",0),task(2,"edge",0),task(3,"above",0),task(4,"tiny",0),task(5,"one",0)},100},
                 {{task(1,"below",499),task(2,"edge",500),task(3,"above",501),task(4,"tiny",3),task(5,"one",4)},500}};
    names={"below","edge","one"}; percentages={100,100,1}; idle={false,false,false};
  } else if (scenario == "cutoff-odd") {
    snapshots = {{{task(1,"edge",0),task(2,"above",0)},100},{{task(1,"edge",126),task(2,"above",127)},201}};
    names={"edge"}; percentages={100}; idle={false};
  } else if (scenario == "wide") {
    snapshots = {{{task(1,"worker",0)},100},{{task(1,"worker",2400000000u)},3000000100u}};
    names={"worker"}; percentages={80}; idle={false};
  } else if (scenario == "cutoff-wrap") {
    // Characterize the existing uint32 cutoff addition, including its wrap;
    // this refactor must not silently change that arithmetic policy.
    snapshots = {{{task(1,"worker",0),task(2,"IDLE0",0)},0},
                 {{task(1,"worker",1),task(2,"IDLE0",0)},3435973837u}};
    names={"IDLE0"}; percentages={0}; idle={true};
  } else {
    snapshots = {{{task(1,"worker",100)},1000},{{task(1,"worker",200)},2000}};
    names={"worker"}; percentages={10}; idle={false};
    if (scenario == "empty-before") snapshots[0].rows.clear();
    if (scenario == "empty-after") snapshots[1].rows.clear();
    if (scenario == "zero-time") snapshots[1].total = snapshots[0].total;
    if (scenario == "zero-tasks") { snapshots[0].rows.clear(); snapshots[1].rows.clear(); }
    if (scenario == "no-visible") { snapshots[1].rows = {task(1,"worker",109),task(2,"new",1000)}; }
    if (scenario == "fail-first") failAllocation=1;
    if (scenario == "fail-second") failAllocation=2;
    available = scenario == "no-visible";
    names.clear(); percentages.clear(); idle.clear();
    loopValid = false; laps = avg = maximum = stalls = allStalls = 0;
  }
  reportedCount = static_cast<UBaseType_t>(snapshots[0].rows.size());
}
static void cpuCases(const std::string& scenario) {
  for (bool json : {false,true}) {
    resetCommand(); std::vector<std::string> names; std::vector<unsigned> percentages; std::vector<bool> idle;
    bool available; cpuFixture(scenario,names,percentages,idle,available);
    jsonCommand = json;
    const char* command = json ? "perftop json" : "perftop";
    const char* result = cmd_perftop(command);
    if (json) {
      CHECK(output.empty()); const auto doc=parse(result);
      CHECK(doc.size()==4 && doc["schema"]==1 && doc["cpuValid"]==available);
      const JsonObjectConst loop=doc["loop"].as<JsonObjectConst>();
      CHECK(loop.size()==6 && loop["valid"]==loopValid && loop["lapsPerSec"]==laps && loop["avgMs"]==avg);
      CHECK(loop["maxMs"]==maximum && loop["stalls5s"]==stalls && loop["totalStalls"]==allStalls);
      const auto rows=doc["tasks"].as<JsonArrayConst>(); CHECK(rows.size()==names.size());
      for(size_t i=0;i<names.size();++i) {
        CHECK(rows[i].size()==3 && rows[i]["name"].as<std::string>()==names[i]);
        CHECK(rows[i]["cpuPct"].as<unsigned>()==percentages[i] && rows[i]["idle"].as<bool>()==idle[i]);
      }
    } else {
      CHECK(std::string(result)=="OK");
      CHECK(output.size()>=7 && output[0].empty() && output[1]=="========== PERFTOP ==========");
      CHECK(output[2]=="fixture loop health" && output[3].empty() && output[4]=="[PERFTOP] live task CPU (0.75s sample):");
      CHECK(output.back()=="========== END PERFTOP ==========");
      if(failAllocation) CHECK(output[5]=="  (cannot allocate task snapshot buffers)");
      else if(!available) CHECK(output[5]=="  (run-time stats unavailable)");
      else {
        size_t row=5; bool anyNonIdle=false;
        for(bool idlePass : {true,false}) for(size_t i=0;i<names.size();++i) {
          if(idle[i]!=idlePass) continue;
          char expected[80]; std::snprintf(expected,sizeof(expected),idlePass ? "  %-16.16s idle %3u%%" : "  %-16.16s %3u%%",names[i].c_str(),percentages[i]);
          CHECK(output[row++]==expected); if(!idlePass) anyNonIdle=true;
        }
        if(!anyNonIdle) CHECK(output[row++]=="  (all non-idle tasks <1% this interval)");
        CHECK(row+1==output.size());
      }
    }
    checkLifetimeAndTiming(json,failAllocation!=0); transcript(command,result);
  }
}
static void statesCase(bool nullName=false) {
  resetCommand(); cpuCommand=false;
  snapshots={{{task(1,"worker",0,eRunning,4,7),task(2,"ready",0,eReady,2,123),
                task(3,"blocked",0,eBlocked,3,64),task(4,"suspended",0,eSuspended,1,18),
                task(5,"deleted",0,eDeleted,0,99),task(6,"unknown-task-with-long-name",0,eInvalid,5,2048)},0}};
  if(nullName) snapshots[0].rows={task(1,nullptr,0,eInvalid)};
  reportedCount=static_cast<UBaseType_t>(snapshots[0].rows.size())+1; // reported vs captured counts differ
  for(bool json : {false,true}) {
    if(nullName && !json) continue; // nullptr printf is outside the text contract.
    output.clear(); snapshotCalls=0;
    const char* command=json ? "taskstats json" : "taskstats";
    const char* result=cmd_taskstats(command);
    CHECK(delayCalls==0 && live.size()==1 && allocCalls==1 && freeCalls==0);
    if(json) {
      CHECK(output.empty()); const auto doc=parse(result); const auto rows=doc["tasks"].as<JsonArrayConst>();
      CHECK(doc.size()==3 && doc["schema"]==1 && doc["total"]==reportedCount);
      CHECK(rows.size()==snapshots[0].rows.size());
      const char* labels[]={"RUN","READY","BLOCK","SUSP","DEL","UNK"};
      for(size_t i=0;i<rows.size();++i) {
        const auto& source=snapshots[0].rows[i];
        CHECK(rows[i]["name"].as<std::string>()==(source.pcTaskName ? source.pcTaskName : "?"));
        CHECK(rows[i]["state"].as<std::string>()==(nullName ? "UNK" : labels[i]));
        CHECK(rows[i]["prio"]==source.uxCurrentPriority && rows[i]["stackFree"]==source.usStackHighWaterMark);
        if(i==0 && !nullName) CHECK(rows[i]["stackSize"]==4096 && rows[i].size()==5);
        else CHECK(rows[i]["stackSize"].isNull() && rows[i].size()==4);
      }
    } else {
      CHECK(std::string(result)=="OK" && output.size()==7);
      CHECK(output[0]=="Task Statistics:\n=================\nTotal Tasks: 7\n\nTask Name          State  Prio  Stack\n================== ===== ===== ======");
      CHECK(output[1]=="worker             RUN      4     7");
      CHECK(output[2]=="ready              READY    2   123");
      CHECK(output[3]=="blocked            BLOCK    3    64");
      CHECK(output[4]=="suspended          SUSP     1    18");
      CHECK(output[5]=="deleted            DEL      0    99");
      CHECK(output[6]=="unknown-task-with- UNK      5  2048");
    }
    transcript(command,result);
  }
}
static void statsBoundaryCase(const std::string& scenario) {
  resetCommand(); cpuCommand=false; reportedCount=scenario=="stats-zero" ? 0 : 2;
  snapshots={{{task(1,"worker",0)},0}};
  if(scenario=="stats-empty") snapshots[0].rows.clear();
  if(scenario=="stats-fail") failAllocation=1;
  const char* result=cmd_taskstats("taskstats json");
  if(scenario=="stats-zero" || scenario=="stats-fail") {
    CHECK(std::string(result)=="Error: Unable to allocate memory for task statistics");
    CHECK(snapshotCalls==0 && live.empty() && allocCalls==(scenario=="stats-zero" ? 0u : 1u));
  } else {
    const auto doc=parse(result); CHECK(doc["total"]==2);
    CHECK(doc["tasks"].size()==(scenario=="stats-empty" ? 0u : 1u));
  }
  CHECK(delayCalls==0); transcript("taskstats json",result);
  if(scenario!="stats-cache") return;
  void* original=live.begin()->first;
  reportedCount=1; snapshotCalls=0;
  result=cmd_taskstats("taskstats json");
  CHECK(live.begin()->first==original && allocCalls==1 && freeCalls==0);
  CHECK(parse(result)["total"]==1); transcript("taskstats json",result);
  reportedCount=3; snapshotCalls=0; failAllocation=2;
  result=cmd_taskstats("taskstats");
  CHECK(std::string(result)=="Error: Unable to allocate memory for task statistics");
  CHECK(live.empty() && freeCalls==1 && snapshotCalls==0); transcript("taskstats",result);
  failAllocation=0;
  result=cmd_taskstats("taskstats json");
  CHECK(allocCalls==3 && live.size()==1 && live.begin()->second.bytes==3*sizeof(TaskStatus_t));
  CHECK(parse(result)["total"]==3); transcript("taskstats json",result);
}
static void validateCase() {
  resetCommand(); validateOnly=true;
  for(const char* command : {"perftop","perftop json","taskstats","taskstats json"}) {
    const char* result=std::strncmp(command,"perftop",7)==0 ? cmd_perftop(command) : cmd_taskstats(command);
    CHECK(std::string(result)=="VALID" && output.empty() && live.empty());
    CHECK(allocCalls==0 && freeCalls==0 && countCalls==0 && snapshotCalls==0 && delayCalls==0);
    CHECK(loopReads==0 && loopPrints==0 && events.empty()); transcript(command,result);
  }
}
int main(int argc,char** argv) {
  CHECK(argc==2); const std::string scenario=argv[1];
  if(scenario=="validate") validateCase();
  else if(scenario=="states") statesCase();
  else if(scenario=="stats-null") statesCase(true);
  else if(scenario.rfind("stats-",0)==0) statsBoundaryCase(scenario);
  else cpuCases(scenario);
  // taskstats deliberately retains one cache until shutdown; reclaim only the
  // fixture-owned allocation after every production invocation has finished.
  while(!live.empty()) snapshotFree(live.begin()->first);
  std::cerr << "checks=" << checks << '\n';
}
