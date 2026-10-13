// Host harness for test_oled_automation_wizard.py. The driver splices in the
// real OLED "New automation" wizard (OLED_Mode_Automations.cpp, from
// `enum AutoWizStep` through wizHandleInput) and its footer toast, plus the
// keyboard length cap and keyboard enums from OLED_Utils.h. Everything else is
// a double: Arduino String, the display (records what is printed where), the
// keyboard open/reset calls, the event catalog, the OLED command executor and
// the navigation events.
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

// ---- Arduino String (only what the spliced code uses) ------------------------
class String {
 public:
  String() = default;
  String(const char* text) : value_(text ? text : "") {}
  String(const std::string& text) : value_(text) {}
  explicit String(unsigned long number) : value_(std::to_string(number)) {}
  const char* c_str() const { return value_.c_str(); }
  size_t length() const { return value_.size(); }
  char operator[](size_t index) const { return index < value_.size() ? value_[index] : '\0'; }
  String& operator+=(const String& other) { value_ += other.value_; return *this; }
  String& operator+=(const char* text) { value_ += text ? text : ""; return *this; }
  String& operator+=(char c) { value_ += c; return *this; }
  friend String operator+(const char* left, const String& right) {
    return String(std::string(left ? left : "") + right.value_);
  }
  void trim() {
    const size_t first = value_.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) { value_.clear(); return; }
    const size_t last = value_.find_last_not_of(" \t\r\n");
    value_ = value_.substr(first, last - first + 1);
  }
  String substring(size_t from) const { return from < value_.size() ? String(value_.substr(from)) : String(); }
  String substring(size_t from, size_t to) const {
    if (from >= value_.size() || to <= from) return String();
    return String(value_.substr(from, to - from));
  }
  long toInt() const { return std::strtol(value_.c_str(), nullptr, 10); }

 private:
  std::string value_;
};

// ---- checks -------------------------------------------------------------------
static int gFailures = 0;
static const char* gCase = "";
#define EXPECT(cond, msg)                                                         \
  do {                                                                            \
    if (!(cond)) {                                                                \
      std::fprintf(stderr, "FAIL [%s] %s  (%s, line %d)\n", gCase,                \
                   std::string(msg).c_str(), #cond, __LINE__);                    \
      ++gFailures;                                                                \
    }                                                                             \
  } while (0)

// ---- platform -------------------------------------------------------------------
#define EXT_RAM_BSS_ATTR
static unsigned long gNowMs = 1000;
static unsigned long millis() { return gNowMs; }

// 128x64 OLED layout (HAL_Display.h: header 10, footer 10).
#define OLED_CONTENT_START_Y 11
#define OLED_CONTENT_HEIGHT 43
#define DISPLAY_COLOR_BLACK 0
#define DISPLAY_COLOR_WHITE 1

struct DrawnText { int x; int y; std::string text; };
class HarnessDisplay {
 public:
  void setTextSize(int) {}
  void setTextColor(uint16_t) {}
  void setTextColor(uint16_t, uint16_t) {}
  void setCursor(int x, int y) { x_ = x; y_ = y; }
  void print(const char* text) { drawn.push_back({x_, y_, text ? text : ""}); }
  void fillRect(int, int, int, int, uint16_t) {}
  std::vector<DrawnText> drawn;

 private:
  int x_ = 0, y_ = 0;
};
static HarnessDisplay gDisplay;
static HarnessDisplay* oledDisplay = &gDisplay;

// ---- input ------------------------------------------------------------------------
struct NavEvents { bool up; bool down; bool left; bool right; };
NavEvents gNavEvents = {false, false, false, false};
static const uint32_t INPUT_BUTTON_A = 1u << 0;
static const uint32_t INPUT_BUTTON_B = 1u << 1;
#define INPUT_CHECK(pressed, button) (((pressed) & (button)) != 0)

// ---- production: keyboard cap and enums (OLED_Utils.h) ------------------------------
// INSERT_KEYBOARD_TYPES

// ---- keyboard doubles ---------------------------------------------------------------
struct KeyboardOpen { std::string title; int maxLength; OLEDKeyboardDictationPolicy policy; };
static std::vector<KeyboardOpen> gKeyboardOpens;
static int gKeyboardResets = 0;
struct HarnessKeyboardState { OLEDKeyboardMode mode = KEYBOARD_MODE_LOWERCASE; };
static HarnessKeyboardState gOledKeyboardState;
static void oledKeyboardInit(const char* title, const char*, int maxLength,
                             OLEDKeyboardDictationPolicy policy) {
  gKeyboardOpens.push_back({title ? title : "", maxLength, policy});
  gOledKeyboardState.mode = KEYBOARD_MODE_LOWERCASE;
}
static void oledKeyboardReset() { ++gKeyboardResets; }

// ---- event catalog doubles (System_EventCatalog.h shapes) ---------------------------
enum class SystemEventFamily : uint8_t { Peer = 1 };
enum class SystemEventKind : uint16_t { PeerOnline = 1 };
struct SystemEventCatalogFamilyInfo { SystemEventFamily id; const char* label; size_t kindCount; };
struct SystemEventCatalogKindInfo { SystemEventKind id; SystemEventFamily family; const char* name; };
static size_t systemEventCatalogFamilyCount() { return 1; }
static bool systemEventCatalogFamilyAt(size_t index, SystemEventCatalogFamilyInfo* out) {
  if (index != 0 || !out) return false;
  *out = {SystemEventFamily::Peer, "Peers", 1};
  return true;
}
static bool systemEventCatalogFamilyKindAt(SystemEventFamily family, size_t index,
                                           SystemEventCatalogKindInfo* out) {
  if (family != SystemEventFamily::Peer || index != 0 || !out) return false;
  *out = {SystemEventKind::PeerOnline, SystemEventFamily::Peer, "peer_online"};
  return true;
}

// ---- OLED command executor double ---------------------------------------------------
static std::vector<std::string> gSent;
static bool executeOLEDCommandWithResult(const String& cmd, char* out, size_t outSize) {
  gSent.emplace_back(cmd.c_str());
  if (out && outSize) std::snprintf(out, outSize, "%s", "OK");
  return true;
}

// ---- Automations list state the toast and wizard touch -------------------------------
static const char* autoActionMsg = nullptr;
static unsigned long autoActionMsgTime = 0;
static bool autoForceRefresh = false;
static const unsigned long AUTO_TOAST_MS = 1500;
static unsigned long gDirtyUntil = 0;
static void oledMarkDirtyUntil(unsigned long untilMs) { if (untilMs > gDirtyUntil) gDirtyUntil = untilMs; }

// ---- production: footer toast and the wizard ----------------------------------------
// INSERT_TOAST
// INSERT_WIZARD

// ---- drivers --------------------------------------------------------------------------
static void resetWorld() {
  gKeyboardOpens.clear();
  gKeyboardResets = 0;
  gSent.clear();
  gDisplay.drawn.clear();
  gNavEvents = {false, false, false, false};
  autoActionMsg = nullptr;
  autoActionMsgTime = 0;
  autoForceRefresh = false;
  gNowMs += 10000;
  oledAutomationsModeResetSessionState();
}

static std::string toast() { return autoActionMsg ? autoActionMsg : "(none)"; }

// "+ New" row pressed: the list handler resets the wizard and opens the name keyboard.
static void startWizard() {
  wizResetAll();
  wizStartKeyboard(AW_NAME, "Name:", false);
}

static void press(uint32_t buttons) {
  gNavEvents = {false, false, false, false};
  wizHandleInput(buttons);
}

static void nav(bool down) {
  gNavEvents = {false, false, false, false};
  (down ? gNavEvents.down : gNavEvents.up) = true;
  wizHandleInput(0);
  gNavEvents = {false, false, false, false};
}

// Name -> Interval -> "5 min" -> command keyboard -> (command) -> CONFIRM.
static void runToConfirm(const std::string& name, const std::string& command) {
  startWizard();
  wizKeyboardDone(String(name));
  EXPECT(sWizStep == AW_TYPE, "a name moves the wizard to the trigger list");
  press(INPUT_BUTTON_A);  // Interval
  press(INPUT_BUTTON_A);  // 5 min -> command keyboard
  EXPECT(sWizStep == AW_COMMAND, "the interval preset opens the command keyboard");
  wizKeyboardDone(String(command));
}

static std::vector<DrawnText> frame() {
  gDisplay.drawn.clear();
  wizDraw();
  return gDisplay.drawn;
}

static std::string frameText(const std::vector<DrawnText>& drawn) {
  std::string all;
  for (const auto& d : drawn) {
    all += "(" + std::to_string(d.x) + "," + std::to_string(d.y) + ")'" + d.text + "' ";
  }
  return all.empty() ? "(empty)" : all;
}

static bool isArrow(const std::string& text) { return text == "\x18" || text == "\x19"; }

static std::string quoted(const std::string& value) {
  std::string out = "\"";
  for (char c : value) {
    if (c == '"') out += '\\';
    out += c;
  }
  return out + "\"";
}

static std::string addLine(const std::string& name, const std::string& command) {
  return "automationadd name=" + quoted(name) + " type=interval intervalms=300000 command=" +
         quoted(command) + " enabled=1";
}

static std::string sent() {
  std::string all;
  for (const auto& line : gSent) all += (all.empty() ? "" : " | ") + line;
  return all.empty() ? "(nothing)" : all;
}

// Rebuild the command from the confirm screen: start at the top, press Down until
// the frame stops changing, and place every drawn row by its absolute index
// (Down presses so far + row on screen). Command rows start "> " (first) or "  ".
static std::string commandShownOnConfirm(int* framesOut) {
  std::map<int, std::string> rows;
  std::vector<DrawnText> current = frame();
  int presses = 0;
  for (;;) {
    for (const auto& d : current) {
      if (d.x != 0 || d.text == "A:Create B:Cancel" || isArrow(d.text)) continue;
      rows[presses + (d.y - OLED_CONTENT_START_Y) / 9] = d.text;
    }
    if (presses >= 40) break;
    nav(true);
    std::vector<DrawnText> next = frame();
    if (frameText(next) == frameText(current)) break;
    current = next;
    ++presses;
  }
  if (framesOut) *framesOut = presses + 1;
  std::string command;
  bool first = true;
  for (const auto& row : rows) {
    const std::string& text = row.second;
    const bool lead = text.compare(0, 2, "> ") == 0;
    const bool more = text.compare(0, 2, "  ") == 0;
    if ((first && lead) || (!first && more)) {
      command += text.substr(2);
      first = false;
    }
  }
  return command;
}

static void checkFrameFits(const std::vector<DrawnText>& drawn) {
  for (const auto& d : drawn) {
    if (isArrow(d.text)) {
      EXPECT(d.x == 122, "scroll arrows sit in column 21, frame: " + frameText(drawn));
      continue;
    }
    EXPECT(d.x + 6 * static_cast<int>(d.text.size()) <= 122,
           "text stays left of the arrow column: '" + d.text + "'");
  }
}

// A 40-character command whose cut at 32 lands inside a quoted path.
static const std::string kCommand40 = "files read path=\"/logs/garden/sen01.txt\"";
// A 40-character name, as dictation produces it.
static const std::string kName40 = "Kitchen window humidity check every hour";

static std::string commandOfLength(size_t length) {
  static const char kAlphabet[] = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
  std::string command = "echo ";
  for (size_t i = 0; command.size() < length; ++i) command += kAlphabet[i % (sizeof(kAlphabet) - 1)];
  return command;
}

// ---- cases ---------------------------------------------------------------------------
static void case_long_command_created_whole() {
  gCase = "long_command_created_whole";
  resetWorld();
  EXPECT(kCommand40.size() == 40, "test command is 40 characters");
  runToConfirm("Garden log", kCommand40);
  EXPECT(sWizStep == AW_CONFIRM, "a command moves the wizard to the confirm screen");
  EXPECT(std::string(sWizCommand) == kCommand40,
         "the wizard keeps the whole 40-character command, kept: '" + std::string(sWizCommand) + "'");
  press(INPUT_BUTTON_A);
  EXPECT(gSent.size() == 1 && gSent[0] == addLine("Garden log", kCommand40),
         "the created automation runs the whole command, sent: " + sent());
  EXPECT(sWizStep == AW_NONE && toast() == "Added", "a successful add closes the wizard, toast: " + toast());
}

static void case_long_name_created_whole() {
  gCase = "long_name_created_whole";
  resetWorld();
  EXPECT(kName40.size() == 40, "test name is 40 characters");
  runToConfirm(kName40, "status");
  EXPECT(std::string(sWizName) == kName40,
         "the wizard keeps the whole 40-character name, kept: '" + std::string(sWizName) + "'");
  press(INPUT_BUTTON_A);
  EXPECT(gSent.size() == 1 && gSent[0] == addLine(kName40, "status"),
         "the created automation carries the whole name, sent: " + sent());
}

static void case_keyboard_capacity() {
  gCase = "keyboard_capacity";
  resetWorld();
  const std::string command = commandOfLength(OLED_KEYBOARD_MAX_LENGTH);
  runToConfirm("Max", command);
  EXPECT(!gKeyboardOpens.empty(), "the wizard opened keyboards");
  for (const auto& open : gKeyboardOpens) {
    EXPECT(open.maxLength >= 1 && static_cast<size_t>(open.maxLength) < sizeof(sWizCommand) &&
               static_cast<size_t>(open.maxLength) < sizeof(sWizName),
           "keyboard '" + open.title + "' admits " + std::to_string(open.maxLength) +
               " characters but the wizard keeps " + std::to_string(sizeof(sWizCommand) - 1));
  }
  const KeyboardOpen& commandKeyboard = gKeyboardOpens.back();
  EXPECT(commandKeyboard.title == "Command:" && commandKeyboard.policy == OLEDKeyboardDictationPolicy::DENY,
         "the command keyboard denies dictation");
  EXPECT(gKeyboardOpens.front().policy == OLEDKeyboardDictationPolicy::ALLOW_PLAINTEXT,
         "the name keyboard admits dictation");
  press(INPUT_BUTTON_A);
  EXPECT(gSent.size() == 1 && gSent[0] == addLine("Max", command),
         "a command of the keyboard's full length is created whole, sent: " + sent());
}

static void case_overlong_refused() {
  gCase = "overlong_refused";
  resetWorld();
  const std::string tooLong(200, 'x');

  startWizard();
  const size_t opens = gKeyboardOpens.size();
  wizKeyboardDone(String(tooLong));
  EXPECT(sWizStep == AW_NAME && gKeyboardOpens.size() == opens + 1 &&
             gKeyboardOpens.back().title == "Name:",
         "an over-long name re-opens the name keyboard");
  EXPECT(sWizName[0] == '\0', "an over-long name is not stored cut short, kept: '" +
                                  std::string(sWizName) + "'");
  EXPECT(toast() == "Too long", "an over-long name is refused with a toast, toast: " + toast());

  resetWorld();
  runToConfirm("Garden log", tooLong);
  EXPECT(sWizStep == AW_COMMAND && gKeyboardOpens.back().title == "Command:",
         "an over-long command re-opens the command keyboard");
  EXPECT(sWizCommand[0] == '\0', "an over-long command is not stored cut short, kept: '" +
                                     std::string(sWizCommand) + "'");
  EXPECT(toast() == "Too long", "an over-long command is refused with a toast, toast: " + toast());
  EXPECT(gSent.empty(), "nothing is created, sent: " + sent());
}

static void case_confirm_shows_whole_command() {
  gCase = "confirm_shows_whole_command";
  for (const std::string& command : {kCommand40, commandOfLength(OLED_KEYBOARD_MAX_LENGTH)}) {
    resetWorld();
    runToConfirm("Garden log", command);
    const std::vector<DrawnText> top = frame();
    checkFrameFits(top);
    EXPECT(!top.empty() && top[0].y == OLED_CONTENT_START_Y && top[0].text == "New: Garden log",
           "the confirm screen opens at its top row, frame: " + frameText(top));
    EXPECT(!top.empty() && top.back().text == "A:Create B:Cancel" &&
               top.back().y == OLED_CONTENT_START_Y + 27,
           "the action line sits under three rows, frame: " + frameText(top));
    int frames = 0;
    const std::string shown = commandShownOnConfirm(&frames);
    EXPECT(shown == command, "the confirm screen shows the whole " + std::to_string(command.size()) +
                                 "-character command, shown: '" + shown + "' over " +
                                 std::to_string(frames) + " frame(s)");
    const std::vector<DrawnText> bottom = frame();
    checkFrameFits(bottom);
    EXPECT(!bottom.empty() && bottom.back().text == "A:Create B:Cancel",
           "the action line stays on screen while scrolled, frame: " + frameText(bottom));
    nav(false);
    EXPECT(frameText(frame()) != frameText(bottom), "Up scrolls back, frame: " + frameText(frame()));
    for (int i = 0; i < 40; ++i) nav(false);
    EXPECT(frameText(frame()) == frameText(top), "Up returns to the top row");
    press(INPUT_BUTTON_A);
    EXPECT(gSent.size() == 1 && gSent[0] == addLine("Garden log", command),
           "A creates from any scroll position, sent: " + sent());
  }

  // A new wizard's confirm screen starts at its top row again.
  resetWorld();
  runToConfirm("Garden log", kCommand40);
  for (int i = 0; i < 5; ++i) nav(true);
  press(INPUT_BUTTON_B);
  EXPECT(sWizStep == AW_NONE, "B cancels the wizard from the confirm screen");
  runToConfirm("Second", commandOfLength(60));
  const std::vector<DrawnText> again = frame();
  EXPECT(!again.empty() && again[0].text == "New: Second",
         "a new confirm screen starts at its top row, frame: " + frameText(again));
}

static void case_short_command_layout_unchanged() {
  gCase = "short_command_layout_unchanged";
  resetWorld();
  runToConfirm("Garden log", "status");
  const std::vector<DrawnText> drawn = frame();
  const std::vector<DrawnText> expected = {
      {0, 11, "New: Garden log"}, {0, 20, "Every 5m"}, {0, 29, "> status"}, {0, 38, "A:Create B:Cancel"}};
  bool same = drawn.size() == expected.size();
  for (size_t i = 0; same && i < drawn.size(); ++i) {
    same = drawn[i].x == expected[i].x && drawn[i].y == expected[i].y && drawn[i].text == expected[i].text;
  }
  EXPECT(same, "a short command draws the four-line confirm screen, frame: " + frameText(drawn));
  nav(true);
  EXPECT(frameText(frame()) == frameText(drawn), "Down does not move a screen that fits");

  // A command of exactly one row still fits without arrows.
  resetWorld();
  runToConfirm("Garden log", "abcdefghijklmnopqr");
  const std::vector<DrawnText> oneRow = frame();
  EXPECT(oneRow.size() == 4 && oneRow[2].text == "> abcdefghijklmnopqr",
         "an 18-character command fills one row, frame: " + frameText(oneRow));
}

int main() {
  case_long_command_created_whole();
  case_long_name_created_whole();
  case_keyboard_capacity();
  case_overlong_refused();
  case_confirm_shows_whole_command();
  case_short_command_layout_unchanged();
  if (gFailures) {
    std::fprintf(stderr, "oled_automation_wizard: %d failure(s)\n", gFailures);
    return 1;
  }
  std::printf("oled_automation_wizard: names and commands kept whole, confirm shows the whole command\n");
  return 0;
}
