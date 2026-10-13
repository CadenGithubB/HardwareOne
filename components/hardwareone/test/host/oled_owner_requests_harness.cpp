// Host harness for test_oled_owner_requests.py: the OLED display commands and
// requestOLEDMode() called from cmd_exec must leave main-loop-owned OLED state
// alone until the owner task's next frame (oledUpdate), and the owner must then
// apply them with the same side effects as before.
//
// The production text is inserted at the INSERT_* markers. A task double
// stands in for FreeRTOS: xTaskGetCurrentTaskHandle() returns kOwnerTask (the
// main loop) or kExecTask (cmd_exec). Fakes record which task touched the
// framebuffer, ran an onEnter hook, read the display role or auth mirror,
// pushed the mode stack or auto-started input.
#include <atomic>
#include <cctype>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#define ENABLE_OLED_DISPLAY 1
#define ENABLE_ESPNOW 0
#define ENABLE_ANO_ENCODER 0
#define EXT_RAM_BSS_ATTR
// Production commands ignore argsInput in validate-only mode, as here.
static bool gCLIValidateOnly = false;
#define RETURN_VALID_IF_VALIDATE_CSTR() \
  do { if (gCLIValidateOnly) { (void)argsInput; return "VALID"; } } while (0)

static void harnessDebugf(const char*, ...) {}
#define DEBUG_DISPLAYF(...) harnessDebugf(__VA_ARGS__)

// ---- Arduino String surface used by the extracted commands ----
class String {
 public:
  String() = default;
  String(const char* text) : text_(text ? text : "") {}
  String(const std::string& text) : text_(text) {}
  String& operator=(const char* text) {
    text_ = text ? text : "";
    return *this;
  }
  size_t length() const { return text_.size(); }
  const char* c_str() const { return text_.c_str(); }
  void trim() {
    size_t begin = 0;
    while (begin < text_.size() && std::isspace(static_cast<unsigned char>(text_[begin]))) begin++;
    size_t end = text_.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(text_[end - 1]))) end--;
    text_ = text_.substr(begin, end - begin);
  }
  void toLowerCase() {
    for (char& c : text_) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  bool startsWith(const char* prefix) const { return text_.rfind(prefix, 0) == 0; }
  bool endsWith(const char* suffix) const {
    const size_t n = std::strlen(suffix);
    return text_.size() >= n && text_.compare(text_.size() - n, n, suffix) == 0;
  }
  String substring(size_t from) const { return from >= text_.size() ? String() : String(text_.substr(from)); }
  String substring(size_t from, size_t to) const {
    if (from >= text_.size() || to <= from) return String();
    return String(text_.substr(from, to - from));
  }
  long toInt() const { return std::strtol(text_.c_str(), nullptr, 10); }
  bool operator==(const char* other) const { return text_ == (other ? other : ""); }
  bool operator==(const String& other) const { return text_ == other.text_; }

 private:
  std::string text_;
};

// ---- FreeRTOS task double ----
using TaskHandle_t = void*;
static int gOwnerTaskObject = 0;
static int gExecTaskObject = 0;
static TaskHandle_t const kOwnerTask = &gOwnerTaskObject;
static TaskHandle_t const kExecTask = &gExecTaskObject;
static TaskHandle_t gCurrentTask = kOwnerTask;
static TaskHandle_t xTaskGetCurrentTaskHandle() { return gCurrentTask; }

// Every side effect that belongs to the OLED owner task, when it runs elsewhere.
static std::vector<std::string> gOffOwnerEffects;
static void ownerEffect(const char* what) {
  if (gCurrentTask != kOwnerTask) gOffOwnerEffects.push_back(what);
}

// ---- portMUX double: records that nothing allocates or frees inside it ----
struct portMUX_TYPE { int held; };
#define portMUX_INITIALIZER_UNLOCKED {0}
static int gCriticalViolations = 0;
static bool gInCritical = false;
static void harnessEnterCritical(portMUX_TYPE* mux) {
  if (mux->held || gInCritical) gCriticalViolations++;
  mux->held = 1;
  gInCritical = true;
}
static void harnessExitCritical(portMUX_TYPE* mux) {
  if (!mux->held) gCriticalViolations++;
  mux->held = 0;
  gInCritical = false;
}
#define portENTER_CRITICAL(mux) harnessEnterCritical(mux)
#define portEXIT_CRITICAL(mux) harnessExitCritical(mux)

// ---- ps_alloc double: counts live text copies ----
enum class AllocPolicy { PreferPSRAM, PreferInternal, PSRAMOnly, InternalOnly };
static int gLiveAllocs = 0;
static void* ps_alloc(size_t size, AllocPolicy, const char*) {
  if (gInCritical) gCriticalViolations++;
  gLiveAllocs++;
  return std::malloc(size);
}
static void ps_free(void* ptr) {
  if (!ptr) return;
  if (gInCritical) gCriticalViolations++;
  gLiveAllocs--;
  std::free(ptr);
}

// INSERT_TYPES

// ---- OLED module state and collaborators ----
enum OLEDAnimationType : int { ANIM_BOOT_PROGRESS = 0, ANIM_STARS = 1, ANIM_WAVE = 2 };
struct OLEDAnimation {
  const char* name;
  const char* description;
  OLEDAnimationType type;
};
static const OLEDAnimation gAnimationRegistry[] = {
  {"boot", "Boot progress", ANIM_BOOT_PROGRESS},
  {"stars", "Starfield", ANIM_STARS},
  {"wave", "Sine wave", ANIM_WAVE},
};
static const int gAnimationCount = 3;

OLEDMode currentOLEDMode = OLED_SYSTEM_STATUS;
String customOLEDText;
OLEDAnimationType currentAnimation = ANIM_BOOT_PROGRESS;
unsigned long animationFrame = 0;
int animationFPS = 10;
bool oledConnected = true;
bool gOledRunning = true;
bool oledBootModeActive = false;
static bool userOverrodeBootMode = false;

struct FakeDisplay {
  int clears = 0;
  int pushes = 0;
  void clearDisplay() { ownerEffect("framebuffer clear"); clears++; }
  void display() { ownerEffect("framebuffer push"); pushes++; }
};
static FakeDisplay gFakeDisplay;
static FakeDisplay* oledDisplay = &gFakeDisplay;
static bool displayIsReady() { return true; }
#define OLED_TRANSACTION(code) do { if (displayIsReady()) { code; } } while (0)

static uint32_t gDirtySeq = 0;
static void oledMarkDirty() { gDirtySeq++; }

static bool gBlockForAuth = false;
static bool shouldBlockForDisplayAuth() {
  ownerEffect("display-auth mirror read");
  return gBlockForAuth;
}
static int gGuestReads = 0;
static bool oledIsGuestSession() {
  ownerEffect("display role read (gLocalDisplayUser)");
  gGuestReads++;
  return false;
}
static bool oledModeAllowedForGuest(OLEDMode) { return true; }
static void oledToastShow(const char*, unsigned long) { ownerEffect("toast"); }

static std::vector<OLEDMode> gModeStack;
static void pushOLEDMode(OLEDMode mode) {
  ownerEffect("mode stack push");
  gModeStack.push_back(mode);
}

struct OLEDModeEntry {
  void (*onEnterFunc)(bool isForward);
  const char* name;
};
static int gOnEnterCalls = 0;
static void fakeOnEnter(bool) {
  ownerEffect("onEnter hook (keyboard/browser resets)");
  gOnEnterCalls++;
}
static const OLEDModeEntry gModeEntry = {fakeOnEnter, "Other"};
static const OLEDModeEntry gCliInputEntry = {fakeOnEnter, "CLI Input"};
static const OLEDModeEntry* findOLEDMode(OLEDMode mode) {
  return mode == OLED_CLI_INPUT ? &gCliInputEntry : &gModeEntry;
}

// The CLI Input module's entry rule (OLEDModeEntry::availFunc), as
// oledModeRuleAllows() reports it; every other mode has none.
static bool gCliInputRefuses = false;
static bool oledModeRuleAllows(OLEDMode mode, String* outReason) {
  ownerEffect("entry rule read (display session)");
  if (mode != OLED_CLI_INPUT || !gCliInputRefuses) return true;
  if (outReason) *outReason = "Login required";
  return false;
}

static const char* getOLEDModeName(OLEDMode mode) {
  switch (mode) {
    case OLED_OFF: return "Off";
    case OLED_MENU: return "Menu";
    case OLED_SYSTEM_STATUS: return "Status";
    case OLED_CUSTOM_TEXT: return "Custom Text";
    case OLED_ANIMATION: return "Animation";
    case OLED_LOGIN: return "Login";
    case OLED_SET_PATTERN: return "Gamepad Password";
    case OLED_CLI_INPUT: return "CLI Input";
    case OLED_UNAVAILABLE: return "Unavail";
    default: return "Other";
  }
}

static OLEDMode modeFromSlug(const String& slug) {
  if (slug == "menu") return OLED_MENU;
  if (slug == "off") return OLED_OFF;
  if (slug == "status") return OLED_SYSTEM_STATUS;
  if (slug == "text") return OLED_CUSTOM_TEXT;
  if (slug == "cliinput") return OLED_CLI_INPUT;
  return (OLEDMode)-1;
}

static int gInputAutoStarts = 0;
static void tryAutoStartInputForMenu() {
  ownerEffect("input auto-start");
  gInputAutoStarts++;
}
static void oledKeyboardDictationTick() { ownerEffect("dictation tick"); }

static int gRenderPasses = 0;
// The real updateOLEDDisplay() keeps this owner guard; its own request drain is
// checked structurally by the suite.
static void updateOLEDDisplay() {
  if (xTaskGetCurrentTaskHandle() != kOwnerTask) {
    oledMarkDirty();
    return;
  }
  gRenderPasses++;
}

static std::vector<std::string> gBroadcasts;
static void broadcastOutput(const char* line) { gBroadcasts.push_back(line ? line : ""); }
static char gDebugBuffer[1024];
static bool ensureDebugBuffer() { return true; }
static char* getDebugBuffer() { return gDebugBuffer; }

static void secureClearString(String& text) { text = ""; }
enum CommandSource { SOURCE_LOCAL_DISPLAY = 1 };
static bool gDisplayAuthed = true;
static bool isTransportAuthenticated(CommandSource) { return gDisplayAuthed; }

using TransportSessionEpoch = uint32_t;
static const TransportSessionEpoch kNoTransportSessionEpoch = 0;

void requestOLEDMode(OLEDMode newMode, const char* reason, bool pushStack = true,
                     bool isBackNav = false);

// Same contract as OLED_Utils.cpp: record the page text, then a replace-in-place
// transition to OLED_UNAVAILABLE.
static int gUnavailableShown = 0;
static std::string gUnavailableTitle;
static std::string gUnavailableReason;
static void enterUnavailablePage(const String& title, const String& reason) {
  gUnavailableShown++;
  gUnavailableTitle = title.c_str();
  gUnavailableReason = reason.c_str();
  requestOLEDMode(OLED_UNAVAILABLE, "unavail.enter", false);
}

// INSERT_OWNER

// Session-boundary double: same generation contract as the production
// function (requested/applied counters, the first snapshot is not a
// replacement); a replacement clears navigation and returns to the menu.
using RaceHook = void (*)();
static RaceHook gAfterBoundaryHook = nullptr;  // cmd_exec work racing the frame
static int gBoundaryResets = 0;
static void oledApplyPendingSessionBoundary() {
  if (!oledIsOwnerTask()) return;
  const uint32_t requested = sOledSessionBoundaryRequested.load(std::memory_order_acquire);
  if (!sOledSessionMirrorInitialized || requested != sOledSessionBoundaryApplied) {
    const bool hadIdentity = sOledSessionMirrorInitialized;
    sOledSessionBoundaryApplied = requested;
    sOledSessionMirrorInitialized = true;
    if (hadIdentity || requested != 0) {
      gBoundaryResets++;
      gModeStack.clear();
      if (currentOLEDMode != OLED_MENU) requestOLEDMode(OLED_MENU, "auth.notify", false);
    }
  }
  if (gAfterBoundaryHook) {
    const RaceHook hook = gAfterBoundaryHook;
    gAfterBoundaryHook = nullptr;
    const TaskHandle_t saved = gCurrentTask;
    gCurrentTask = kExecTask;
    hook();
    gCurrentTask = saved;
  }
}

// INSERT_TRANSITION

// INSERT_SETPATTERN

// ---- checks ----
static int gChecks = 0;
static int gFailures = 0;
static void check(bool ok, const std::string& what) {
  gChecks++;
  if (!ok) {
    gFailures++;
    std::printf("FAIL: %s\n", what.c_str());
  }
}

static std::string offOwnerList() {
  std::string out;
  for (const std::string& effect : gOffOwnerEffects) {
    if (!out.empty()) out += ", ";
    out += effect;
  }
  return out.empty() ? "none" : out;
}

static void checkNothingOffOwner(const std::string& when) {
  check(gOffOwnerEffects.empty(),
        when + ": owner-task state touched on cmd_exec: " + offOwnerList());
  gOffOwnerEffects.clear();
}

static void ownerFrame() {
  const TaskHandle_t saved = gCurrentTask;
  gCurrentTask = kOwnerTask;
  oledUpdate();
  gCurrentTask = saved;
}

static void onExec() { gCurrentTask = kExecTask; }

static void resetCase(OLEDMode mode) {
  ownerFrame();  // drains anything an earlier case left queued
  gCurrentTask = kOwnerTask;
  currentOLEDMode = mode;
  customOLEDText = "old text";
  currentAnimation = ANIM_BOOT_PROGRESS;
  animationFrame = 42;
  gModeStack.clear();
  gOffOwnerEffects.clear();
  gBroadcasts.clear();
  gFakeDisplay = FakeDisplay();
  gOnEnterCalls = 0;
  gInputAutoStarts = 0;
  gGuestReads = 0;
  gBlockForAuth = false;
  gDisplayAuthed = true;
  sStep = PATTERN_STEP_CONFIRM;
  sFirstPattern = "^^vv";
  gCliInputRefuses = false;
  gUnavailableShown = 0;
  gUnavailableTitle.clear();
  gUnavailableReason.clear();
}

static bool broadcastSaid(const char* line) {
  for (const std::string& said : gBroadcasts) {
    if (said == line) return true;
  }
  return false;
}

static void caseCustomText() {
  resetCase(OLED_CUSTOM_TEXT);
  onExec();
  const char* reply = cmd_oledtext("\"new text\"");
  check(std::strcmp(reply, "OK") == 0, "oledtext replies OK");
  check(customOLEDText == "old text",
        "off-owner oledtext must not reassign customOLEDText before the owner frame "
        "(displayCustomText prints it on the main loop)");
  check(broadcastSaid("Custom text set: new text"), "oledtext reports the text it set");
  checkNothingOffOwner("oledtext");
  const uint32_t dirtyBefore = gDirtySeq;
  ownerFrame();
  check(customOLEDText == "new text", "the owner frame applies the oledtext text");
  check(currentOLEDMode == OLED_CUSTOM_TEXT, "oledtext shows the custom-text page");
  check(gDirtySeq != dirtyBefore, "oledtext repaints even when already on the page");
  check(gLiveAllocs == 0, "the oledtext copy is freed once applied");
  checkNothingOffOwner("oledtext owner frame");
}

static void caseModeMenu() {
  resetCase(OLED_SYSTEM_STATUS);
  onExec();
  check(std::strcmp(cmd_oledmode("menu"), "OK") == 0, "oledmode menu replies OK");
  check(currentOLEDMode == OLED_SYSTEM_STATUS,
        "off-owner oledmode must not write currentOLEDMode before the owner frame");
  check(gOnEnterCalls == 0, "off-owner oledmode must not run the target's onEnter hook");
  check(gInputAutoStarts == 0, "off-owner oledmode must not auto-start input on cmd_exec");
  check(broadcastSaid("OLED mode: Menu"), "oledmode reports the requested mode");
  checkNothingOffOwner("oledmode menu");
  ownerFrame();
  check(currentOLEDMode == OLED_MENU, "the owner frame enters the requested mode");
  check(gOnEnterCalls == 1, "the owner runs the onEnter hook once");
  check(gInputAutoStarts == 1, "oledmode menu still auto-starts input (on the owner)");
  check(gModeStack.empty(), "oledmode does not push the back stack");
  checkNothingOffOwner("oledmode menu owner frame");
}

static void caseModeOff() {
  resetCase(OLED_SYSTEM_STATUS);
  onExec();
  check(std::strcmp(cmd_oledmode("off"), "OK") == 0, "oledmode off replies OK");
  check(gFakeDisplay.clears == 0 && gFakeDisplay.pushes == 0,
        "off-owner oledmode off must not clear or push the framebuffer");
  check(currentOLEDMode == OLED_SYSTEM_STATUS, "oledmode off waits for the owner frame");
  checkNothingOffOwner("oledmode off");
  ownerFrame();
  check(currentOLEDMode == OLED_OFF, "the owner frame turns the page off");
  check(gFakeDisplay.clears == 1 && gFakeDisplay.pushes == 1,
        "the owner blanks and pushes the panel for oledmode off");
  checkNothingOffOwner("oledmode off owner frame");

  resetCase(OLED_SYSTEM_STATUS);
  onExec();
  check(std::strcmp(cmd_oledmode("bogus"), "ERROR") == 0, "an unknown oledmode slug is refused");
  ownerFrame();
  check(currentOLEDMode == OLED_SYSTEM_STATUS && gOnEnterCalls == 0,
        "a refused oledmode posts nothing");
}

static void caseClear() {
  resetCase(OLED_SYSTEM_STATUS);
  onExec();
  check(std::strcmp(cmd_oledclear(""), "OK") == 0, "oledclear replies OK");
  check(broadcastSaid("OLED display cleared"), "oledclear keeps its reply");
  check(gFakeDisplay.clears == 0 && gFakeDisplay.pushes == 0,
        "off-owner oledclear must not clear or push the framebuffer");
  checkNothingOffOwner("oledclear");
  const uint32_t dirtyBefore = gDirtySeq;
  ownerFrame();
  check(gFakeDisplay.clears == 1 && gFakeDisplay.pushes == 1,
        "the owner blanks and pushes the panel for oledclear");
  check(currentOLEDMode == OLED_SYSTEM_STATUS, "oledclear keeps the mode");
  check(gDirtySeq == dirtyBefore, "oledclear does not mark the page dirty (the blank stays)");
  checkNothingOffOwner("oledclear owner frame");
}

static void caseAnimation() {
  resetCase(OLED_SYSTEM_STATUS);
  onExec();
  check(std::strcmp(cmd_oledanim("stars"), "OK") == 0, "oledanim stars replies OK");
  check(currentAnimation == ANIM_BOOT_PROGRESS && animationFrame == 42 &&
            currentOLEDMode == OLED_SYSTEM_STATUS,
        "off-owner oledanim must not change the animation, its frame or the mode "
        "before the owner frame");
  checkNothingOffOwner("oledanim");
  ownerFrame();
  check(currentAnimation == ANIM_STARS, "the owner selects the requested animation");
  check(currentOLEDMode == OLED_ANIMATION, "oledanim enters the animation page");
  check(animationFrame == 0, "oledanim restarts the animation");
  checkNothingOffOwner("oledanim owner frame");
}

static void caseSetPattern() {
  resetCase(OLED_MENU);
  onExec();
  check(std::strcmp(cmd_setpattern(""), "Opening gamepad password setup on OLED...") == 0,
        "setgamepadpassword keeps its reply");
  check(sStep == PATTERN_STEP_CONFIRM && sFirstPattern == "^^vv",
        "off-owner setgamepadpassword must not reset the pattern flow the owner's input "
        "handler is comparing");
  check(currentOLEDMode == OLED_MENU && gModeStack.empty(),
        "off-owner setgamepadpassword must not switch mode or push the back stack");
  checkNothingOffOwner("setgamepadpassword");
  ownerFrame();
  check(sStep == PATTERN_STEP_AUTH_CHECK && sFirstPattern == "",
        "the owner restarts the pattern flow");
  check(currentOLEDMode == OLED_SET_PATTERN, "the owner opens the pattern page");
  check(gModeStack.size() == 1 && gModeStack[0] == OLED_MENU,
        "setgamepadpassword pushes the previous mode (B returns there)");
  checkNothingOffOwner("setgamepadpassword owner frame");

  resetCase(OLED_MENU);
  gDisplayAuthed = false;
  onExec();
  check(std::strncmp(cmd_setpattern(""), "Error: Log in on OLED first", 27) == 0,
        "setgamepadpassword still needs an OLED login");
  ownerFrame();
  check(currentOLEDMode == OLED_MENU && sStep == PATTERN_STEP_CONFIRM,
        "a refused setgamepadpassword posts nothing");
}

static void casePlainRequest() {
  // oledenabled and oledstart reach requestOLEDMode() on cmd_exec.
  resetCase(OLED_MENU);
  onExec();
  requestOLEDMode(OLED_SYSTEM_STATUS, "cmd.oledenabled.forceDefault", false);
  check(currentOLEDMode == OLED_MENU && gOnEnterCalls == 0 && gGuestReads == 0,
        "off-owner requestOLEDMode must not transition, run onEnter or read the display "
        "role before the owner frame");
  checkNothingOffOwner("requestOLEDMode");
  ownerFrame();
  check(currentOLEDMode == OLED_SYSTEM_STATUS && gOnEnterCalls == 1,
        "the owner applies an off-owner requestOLEDMode");
  check(gGuestReads > 0, "the guest gate is evaluated on the owner");
  checkNothingOffOwner("requestOLEDMode owner frame");

  resetCase(OLED_SYSTEM_STATUS);
  gBlockForAuth = true;
  onExec();
  requestOLEDMode(OLED_MENU, "cli", false);
  ownerFrame();
  check(currentOLEDMode == OLED_LOGIN, "the display-auth gate still redirects to the login page");
  checkNothingOffOwner("requestOLEDMode auth gate");
}

static void caseOrder() {
  resetCase(OLED_SYSTEM_STATUS);
  onExec();
  cmd_oledmode("menu");
  cmd_setpattern("");
  ownerFrame();
  check(currentOLEDMode == OLED_SET_PATTERN && gModeStack.size() == 1 &&
            gModeStack[0] == OLED_MENU,
        "requests apply in post order (oledmode menu, then setgamepadpassword pushes Menu)");

  resetCase(OLED_SYSTEM_STATUS);
  onExec();
  cmd_oledtext("first");
  cmd_oledtext("second");
  ownerFrame();
  check(customOLEDText == "second" && gLiveAllocs == 0, "the last oledtext wins, no copy leaks");
  checkNothingOffOwner("ordered requests");
}

static void caseBoundary() {
  // A command posted before a login/logout: its data is handed over, its
  // navigation is not applied after the session boundary.
  resetCase(OLED_SYSTEM_STATUS);
  onExec();
  cmd_oledtext("before logout");
  sOledSessionBoundaryRequested.fetch_add(1, std::memory_order_release);
  ownerFrame();
  check(currentOLEDMode == OLED_MENU,
        "a session boundary after the post keeps the boundary's screen");
  check(customOLEDText == "before logout" && gLiveAllocs == 0,
        "a superseded oledtext still hands over its text");

  resetCase(OLED_SYSTEM_STATUS);
  onExec();
  cmd_setpattern("");
  sOledSessionBoundaryRequested.fetch_add(1, std::memory_order_release);
  ownerFrame();
  check(currentOLEDMode == OLED_MENU && gModeStack.empty(),
        "a superseded setgamepadpassword does not open the page for the next session");
  check(sStep == PATTERN_STEP_AUTH_CHECK && sFirstPattern == "",
        "a superseded setgamepadpassword still resets the flow");

  // A command posted after a boundary the frame has not applied yet runs after it.
  resetCase(OLED_SYSTEM_STATUS);
  gAfterBoundaryHook = [] {
    sOledSessionBoundaryRequested.fetch_add(1, std::memory_order_release);
    cmd_oledmode("status");
  };
  currentOLEDMode = OLED_CUSTOM_TEXT;
  ownerFrame();
  ownerFrame();
  check(currentOLEDMode == OLED_SYSTEM_STATUS,
        "a request posted after a pending session boundary applies after that boundary");
  checkNothingOffOwner("session boundary");
}

static void caseOverflow() {
  resetCase(OLED_SYSTEM_STATUS);
  onExec();
  char text[16];
  for (int i = 0; i < 9; i++) {
    std::snprintf(text, sizeof(text), "t%d", i);
    cmd_oledtext(text);
  }
  check(customOLEDText == "old text", "queued oledtext requests wait for the owner frame");
  check(gLiveAllocs == 8, "a full queue drops (and frees) the oldest request");
  ownerFrame();
  check(customOLEDText == "t8" && gLiveAllocs == 0, "the newest request survives an overflow");
  checkNothingOffOwner("overflow");
}

static void caseOwnerCalls() {
  resetCase(OLED_SYSTEM_STATUS);
  gCurrentTask = kOwnerTask;
  cmd_oledtext("now");
  check(customOLEDText == "now" && currentOLEDMode == OLED_CUSTOM_TEXT && gLiveAllocs == 0,
        "on the owner task oledtext applies at once");
  cmd_oledmode("menu");
  check(currentOLEDMode == OLED_MENU && gInputAutoStarts == 1,
        "on the owner task oledmode applies at once");
  requestOLEDMode(OLED_SYSTEM_STATUS, "menu.select", true);
  check(currentOLEDMode == OLED_SYSTEM_STATUS && gModeStack.size() == 1,
        "owner-side requestOLEDMode is immediate and pushes the stack");
}

static void caseModeRule() {
  // oledmode honors the target mode's own entry rule (its registered availFunc).
  resetCase(OLED_SYSTEM_STATUS);
  gCliInputRefuses = true;
  onExec();
  check(std::strcmp(cmd_oledmode("cliinput"), "OK") == 0, "oledmode cliinput replies OK");
  check(currentOLEDMode == OLED_SYSTEM_STATUS && gUnavailableShown == 0,
        "oledmode with a refusing entry rule waits for the owner frame");
  checkNothingOffOwner("oledmode cliinput (rule refuses)");
  ownerFrame();
  check(currentOLEDMode == OLED_UNAVAILABLE,
        "oledmode to a mode whose own entry rule refuses (CLI Input, no named display "
        "session) must show the unavailable page instead of entering the mode");
  check(gUnavailableTitle == "CLI Input" && gUnavailableReason == "Login required",
        "the unavailable page names the mode and the module's reason (title '" +
            gUnavailableTitle + "', reason '" + gUnavailableReason + "')");
  check(gModeStack.size() == 1 && gModeStack[0] == OLED_SYSTEM_STATUS,
        "a refused oledmode pushes the screen it was on (B and the timeout return there)");
  checkNothingOffOwner("oledmode cliinput owner frame (rule refuses)");

  resetCase(OLED_SYSTEM_STATUS);
  gCliInputRefuses = true;
  gBlockForAuth = true;
  onExec();
  cmd_oledmode("cliinput");
  ownerFrame();
  check(currentOLEDMode == OLED_LOGIN && gModeStack.empty() && gUnavailableShown == 0,
        "with display auth pending, oledmode still lands on the login page without a stack "
        "push, whatever the mode's rule says");

  resetCase(OLED_SYSTEM_STATUS);
  onExec();
  cmd_oledmode("cliinput");
  ownerFrame();
  check(currentOLEDMode == OLED_CLI_INPUT && gModeStack.empty() && gUnavailableShown == 0 &&
            gOnEnterCalls == 1,
        "oledmode enters a mode whose rule allows it, as before");

  resetCase(OLED_SYSTEM_STATUS);
  gCliInputRefuses = true;
  requestOLEDMode(OLED_CLI_INPUT, "menu.select", true);
  check(currentOLEDMode == OLED_CLI_INPUT && gUnavailableShown == 0,
        "requestOLEDMode itself is not gated by entry rules (back-nav, X:Start, login flows)");
}

int main() {
  gCurrentTask = kOwnerTask;
  oledBindOwnerTask();
  (void)sOledUiSessionEpoch;
  (void)sOledUiSessionAuthed;
  (void)animationFPS;
  (void)userOverrodeBootMode;

  caseCustomText();
  caseModeMenu();
  caseModeOff();
  caseClear();
  caseAnimation();
  caseSetPattern();
  casePlainRequest();
  caseOrder();
  caseBoundary();
  caseOverflow();
  caseOwnerCalls();
  caseModeRule();
  ownerFrame();
  check(gLiveAllocs == 0, "no text copy outlives the queue");
  check(gCriticalViolations == 0, "nothing allocates or frees inside the request spinlock");

  std::printf("oled_owner_requests: %d/%d checks passed\n", gChecks - gFailures, gChecks);
  return gFailures == 0 ? 0 : 1;
}
