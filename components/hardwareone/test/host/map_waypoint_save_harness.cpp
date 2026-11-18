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

bool gCliValidateOnly = false;
static char gDebugBuffer[1024];
static bool ensureDebugBuffer() { return true; }
static char* getDebugBuffer() { return gDebugBuffer; }
enum class AllocPref { RequirePsram, PreferPsram };
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

// An argument that is not a number is refused; it is never read as 0.
static void caseJunkArguments() {
  reset();
  waypoint("add 1 1 First");
  const std::string notANumber = kCommandReplyNotANumber;
  for (const char* args : {"del abc", "del 1.5", "del First", "goto x", "rename First Cabin", "notes x text"}) {
    EXPECT(waypoint(args) == notANumber, args);
  }
  const Waypoint* first = WaypointManager::getWaypoint(0);
  EXPECT(first && std::strcmp(first->name, "First") == 0 && first->notes[0] == '\0' &&
         WaypointManager::getSelectedTarget() < 0,
         "junk indexes leave waypoint 0 and the target alone");

  const int saves = gSaveCalls;
  for (const char* args : {"add abc def Home", "add 12abc 5 Home", "add 999 999 Far", "add 1e1 2 Exp",
                           "add nan 2 Nan", "add 91 0 North", "add 0 -180.5 West"}) {
    const std::string out = waypoint(args);
    EXPECT(out.rfind("Error: Latitude and longitude", 0) == 0, args);
  }
  EXPECT(WaypointManager::getActiveCount() == 1 && gSaveCalls == saves, "a refused add writes nothing");
  EXPECT(waypoint("add -0.0 180 My Home") == "Added waypoint 1: My Home", "a name keeps its words");
  // A quoted name is stored without its quotes, so the name lookups find it.
  EXPECT(waypoint("add 1 1 \"Lake Hut\"") == "Added waypoint 2: Lake Hut" &&
         WaypointManager::findWaypointByName("Lake Hut") == 2,
         "a quoted name is stored without its quotes");
  EXPECT(waypoint("add 1 1 \"\"") == "Added waypoint 3: WP" && WaypointManager::findWaypointByName("WP") == 3,
         "an empty quoted name falls back to WP");

  const std::string lon = cmd_waypointfile(String("\"/sd/photo.jpg\" 47.6 abc Cabin"));
  EXPECT(lon.rfind("Error: Longitude is not a decimal number", 0) == 0, "waypointfile refuses a junk longitude");
  const std::string range = cmd_waypointfile(String("\"/sd/photo.jpg\" 95 10 Cabin"));
  EXPECT(range.rfind("Error: Latitude and longitude", 0) == 0, "waypointfile refuses a latitude out of range");
  EXPECT(WaypointManager::findWaypointByName("Cabin") < 0, "a refused waypointfile creates no waypoint");
}

// rename, notes, clearall and a file link report a write that failed and undo
// the change; a navigation target stays set and its reply says it is not saved.
static void caseChangeNotSaved() {
  reset();
  waypoint("add 1 1 First");
  waypoint("notes 0 old notes");
  gSaveOk = false;
  for (const char* args : {"rename 0 Cabin", "notes 0 new notes", "clearall"}) {
    const std::string out = waypoint(args);
    EXPECT(out.rfind("Error:", 0) == 0 && (contains(out, "not saved") || contains(out, "not cleared")), args);
  }
  const Waypoint* first = WaypointManager::getWaypoint(0);
  EXPECT(first && std::strcmp(first->name, "First") == 0 && std::strcmp(first->notes, "old notes") == 0,
         "a change that was not saved is undone");
  const std::string link = cmd_waypointfile(String("\"/sd/photo.jpg\" First"));
  EXPECT(link.rfind("Error:", 0) == 0 && contains(link, "not saved") && WaypointManager::getFileCount(0) == 0,
         "a file link that was not saved is dropped");
  const std::string target = waypoint("goto 0");
  EXPECT(target.rfind("Navigation target: First", 0) == 0 && contains(target, "not saved") &&
         WaypointManager::getSelectedTarget() == 0,
         "goto sets the target and says it is not saved");
  EXPECT(waypoint("rename 9 Cabin") == "Error: Invalid waypoint index", "an empty index is refused before any write");

  gSaveOk = true;
  EXPECT(waypoint("rename 0 Cabin") == "Renamed waypoint 0: Cabin", "a saved rename keeps its reply");
  EXPECT(waypoint("clearall") == "All waypoints cleared" && WaypointManager::getActiveCount() == 0,
         "a saved clearall keeps its reply");
}

int main() {
  caseSuccessUnchanged();
  caseAddNotSaved();
  caseDeleteNotSaved();
  caseWaypointFileNotSaved();
  caseJunkArguments();
  caseChangeNotSaved();
  if (failures) {
    std::fprintf(stderr, "waypoint save: %d of %u checks failed\n", failures, checks);
    return 1;
  }
  std::printf("waypoint save: %u checks passed\n", checks);
  return 0;
}
