// Waypoint owner save reporting. test_oled_map_actions.py inserts the shipping
// WaypointManager declaration and mutators, cmd_waypoint, cmd_waypointfile,
// CommandArgs and requireQuotedToken. saveWaypoints() is the boundary: a double
// whose result the cases choose (the real one also fails with no map loaded
// or when the VFS refuses the write).
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "map_actions_test_string.h"

#define ERROR_MAPSF(...) do {} while (0)
#define WARN_MAPSF(...) do {} while (0)
#define INFO_MAPSF(...) do {} while (0)

static int failures;
static unsigned checks;
#define EXPECT(condition, what) do { ++checks; if (!(condition)) { ++failures; \
  std::fprintf(stderr, "FAIL: %s\n      %s:%d: %s\n", what, __FILE__, __LINE__, #condition); } } while (0)

bool gCLIValidateOnly = false;
static char gDebugBuffer[1024];
static bool ensureDebugBuffer() { return true; }
static char* getDebugBuffer() { return gDebugBuffer; }
enum class AllocPref { RequirePSRAM, PreferPSRAM };
static void* ps_alloc(size_t size, AllocPref, const char*) { return std::calloc(1, size); }
struct AuthContext {};
static const AuthContext& currentAuthContext() { static const AuthContext ctx; return ctx; }
namespace VFS {
static bool existsGuarded(const char*, const AuthContext&) { return true; }
}  // namespace VFS
class MapRenderer;

// INSERT_TYPES

static bool gSaveOk = true;
static int gSaveCalls;
bool WaypointManager::saveWaypoints() {
  ++gSaveCalls;
  return gSaveOk;
}

// INSERT_PRODUCTION

static std::string waypoint(const char* args) { return cmd_waypoint(String(args)); }
static bool contains(const std::string& text, const char* part) { return text.find(part) != std::string::npos; }

static void reset() {
  gSaveOk = true;
  WaypointManager::clearAll();
  gSaveCalls = 0;
}

static void caseSuccessUnchanged() {
  reset();
  EXPECT(waypoint("add 40.5 -74.25 Camp") == "Added waypoint 0: Camp" && gSaveCalls == 1,
         "a saved add keeps its reply");
  EXPECT(WaypointManager::getActiveCount() == 1, "the saved waypoint is listed");
  EXPECT(waypoint("del 0") == "Deleted waypoint 0" && WaypointManager::getActiveCount() == 0,
         "a saved delete keeps its reply");
  EXPECT(waypoint("del 5") == "Error: Invalid waypoint index", "an empty index is still refused");
  EXPECT(waypoint("del 99") == "Error: Invalid waypoint index", "an out-of-range index is still refused");
}

static void caseAddNotSaved() {
  reset();
  gSaveOk = false;
  const std::string out = waypoint("add 1.5 2.5 Lost");
  EXPECT(out.rfind("Error:", 0) == 0 && contains(out, "not saved"),
         "waypoint add reports a write that failed");
  EXPECT(WaypointManager::getActiveCount() == 0 && !WaypointManager::getWaypoint(0),
         "a waypoint that was not saved is not kept in memory");
  EXPECT(WaypointManager::addWaypoint(1.0f, 2.0f, "Api", "notes") < 0 &&
         WaypointManager::addWaypoint(1.0f, 2.0f, "Api") < 0,
         "addWaypoint returns a failure when the write fails");
  EXPECT(WaypointManager::getActiveCount() == 0, "failed adds leave the table unchanged");

  // The slot released by a failed add is reused by the next saved one.
  gSaveOk = true;
  EXPECT(waypoint("add 3 4 Kept") == "Added waypoint 0: Kept", "the next saved add takes the first slot");
}

static void caseDeleteNotSaved() {
  reset();
  waypoint("add 1 1 First");
  waypoint("add 2 2 Second");
  waypoint("goto 1");
  EXPECT(WaypointManager::getSelectedTarget() == 1, "fixture: navigation target set");
  gSaveOk = false;
  const std::string out = waypoint("del 1");
  EXPECT(out.rfind("Error:", 0) == 0 && contains(out, "not saved"),
         "waypoint del reports a write that failed");
  EXPECT(WaypointManager::getWaypoint(1) && WaypointManager::getSelectedTarget() == 1,
         "a deletion that was not saved keeps the waypoint and the target");
  EXPECT(!WaypointManager::deleteWaypoint(0) && WaypointManager::getWaypoint(0),
         "deleteWaypoint returns false when the write fails");
  EXPECT(waypoint("del 7") == "Error: Invalid waypoint index", "an empty index is refused before any write");
}

static void caseWaypointFileNotSaved() {
  reset();
  gSaveOk = false;
  const std::string out = cmd_waypointfile(String("\"/sd/photo.jpg\" 40.5 -74.25 Spot"));
  EXPECT(out.rfind("Error:", 0) == 0 && contains(out, "not saved"),
         "waypointfile reports a waypoint it could not save");
  EXPECT(WaypointManager::getActiveCount() == 0, "nothing is kept");
  gSaveOk = true;
  const std::string ok = cmd_waypointfile(String("\"/sd/photo.jpg\" 40.5 -74.25 Spot"));
  EXPECT(ok.rfind("Linked /sd/photo.jpg to 'Spot'", 0) == 0, "a saved waypointfile still links the file");
}

int main() {
  caseSuccessUnchanged();
  caseAddNotSaved();
  caseDeleteNotSaved();
  caseWaypointFileNotSaved();
  if (failures) {
    std::fprintf(stderr, "waypoint save: %d of %u checks failed\n", failures, checks);
    return 1;
  }
  std::printf("waypoint save: %u checks passed\n", checks);
  return 0;
}
