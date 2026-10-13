// Host harness for test_oled_power_settings.py. The driver splices production
// code in at the INSERT_* markers: the Power > Sleep Settings row action and its
// input handler (OLED_Mode_Power.cpp), the OLED action helper (OLED_Utils.cpp
// oledRunCommand, when the tree has it) and the real `powersave` handler
// (System_Power.cpp cmd_powersave). Doubles: the OLED command executor (admin
// gate with the flag of the production `powersave` row, then cmd_powersave, plus
// a [CMD] audit record), setSetting (records who wrote), the global toast, the
// guest gate, the scroll state and the input bits.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#ifndef HW1_POWERSAVE_REQUIRES_ADMIN
#error "the driver passes the production powersave row's requiresAdmin flag"
#endif

// ---- Arduino String (only what the spliced code uses) ----------------------
class String {
 public:
  String() = default;
  String(const char* text) : value_(text ? text : "") {}
  String(const std::string& text) : value_(text) {}
  const char* c_str() const { return value_.c_str(); }
  size_t length() const { return value_.size(); }
  long toInt() const { return std::strtol(value_.c_str(), nullptr, 10); }
  void trim() {
    const char* blanks = " \t\r\n";
    const size_t first = value_.find_first_not_of(blanks);
    if (first == std::string::npos) { value_.clear(); return; }
    value_ = value_.substr(first, value_.find_last_not_of(blanks) - first + 1);
  }

 private:
  std::string value_;
};

// ---- checks --------------------------------------------------------------------
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

// ---- settings: the one field these screens touch -----------------------------------
struct HarnessSettings { uint32_t powerSaveTimeoutMinutes = 10; };
static HarnessSettings gSettings;

// Each setSetting call is recorded with whether it ran inside a submitted command
// (cmd_powersave under the executor) or directly from the OLED task.
struct SettingWrite { bool viaCommand; uint32_t from; uint32_t to; };
static std::vector<SettingWrite> gWrites;
static bool gInCommand = false;
static void setSetting(uint32_t& field, const uint32_t& value) {
  gWrites.push_back({gInCommand, field, value});
  field = value;
}

// ---- debug buffer (System_Debug) -----------------------------------------------------
static char gDebugBuffer[1024];
static bool ensureDebugBuffer() { return true; }
static char* getDebugBuffer() { return gDebugBuffer; }

// ---- production: the powersave command handler (System_Power.cpp) -------------------
// INSERT_CMD_POWERSAVE

// ---- OLED command executor doubles ------------------------------------------------------
// executeOLEDCommandWithResult models submitAndExecuteSync for `powersave`: the
// admin gate (its real refusal text) when the production row is admin-only, then
// the real handler, and a [CMD] audit record for an executed command.
static bool gAdmin = false;
static std::vector<std::string> gSubmitted;      // executeOLEDCommandWithResult lines
static std::vector<std::string> gFireAndForget;  // executeOLEDCommand lines
static std::vector<std::string> gAudit;
bool executeOLEDCommandWithResult(const String& line, char* out, size_t outSize) {
  const std::string text = line.c_str();
  gSubmitted.push_back(text);
  const size_t space = text.find(' ');
  const std::string name = text.substr(0, space);
  const std::string args = space == std::string::npos ? "" : text.substr(space + 1);
  std::string reply;
  bool ok = false;
  if (name != "powersave") {
    reply = "Error: Unknown command '" + name + "'";
  } else if (HW1_POWERSAVE_REQUIRES_ADMIN && !gAdmin) {
    reply = "Error: Admin access required for command 'powersave'. Contact an administrator.";
  } else {
    gInCommand = true;
    reply = cmd_powersave(String(args));
    gInCommand = false;
    gAudit.push_back("[CMD] " + text);
    ok = reply.rfind("Error", 0) != 0;
  }
  if (out && outSize) std::snprintf(out, outSize, "%s", reply.c_str());
  return ok;
}
void executeOLEDCommand(const String& line) { gFireAndForget.emplace_back(line.c_str()); }

// ---- global toast (OLED_UI.h) -------------------------------------------------------
enum class OledUIIcon : uint8_t { NONE, INFO, SUCCESS, WARNING, ERROR, QUESTION };
static std::string gToast;
static int gToastCount = 0;
static void oledToastShow(const char* message, uint32_t = 3000, OledUIIcon = OledUIIcon::NONE) {
  gToast = message ? message : "";
  ++gToastCount;
}

// ---- production: the OLED action helper (OLED_Utils.cpp) ------------------------------
#if HW1_HAVE_RUN_COMMAND
// INSERT_RUN_COMMAND
#endif

// ---- Power screen doubles -------------------------------------------------------------
struct OLEDScrollState { int selectedIndex = 0; };
static OLEDScrollState sPowerSleepScroll;
static void initPowerScrollStates() {}
static bool oledScrollHandleNav(OLEDScrollState*, bool = false) { return false; }
static const uint32_t INPUT_BUTTON_A = 1u << 0;
static const uint32_t INPUT_BUTTON_B = 1u << 1;
static const uint32_t INPUT_BUTTON_X = 1u << 2;
#define INPUT_CHECK(state, btn) (((state) & (btn)) != 0)
static bool gGuest = false;
static bool oledGuestBlocksMutate() {
  if (!gGuest) return false;
  oledToastShow("View-only", 2000);
  return true;
}

// ---- production: the Sleep Settings rows (OLED_Mode_Power.cpp) ------------------------
// INSERT_POWER_SLEEP

// ---- cases ------------------------------------------------------------------------------
static void resetWorld() {
  gSettings = HarnessSettings();
  gWrites.clear();
  gInCommand = false;
  gAdmin = false;
  gSubmitted.clear();
  gFireAndForget.clear();
  gAudit.clear();
  gToast.clear();
  gToastCount = 0;
  sPowerSleepScroll = OLEDScrollState();
  gGuest = false;
}

static void pressRow(int row, uint32_t button = INPUT_BUTTON_A) {
  sPowerSleepScroll.selectedIndex = row;
  EXPECT(powerSleepInputHandler(0, 0, button), "the Sleep screen consumes A/X on a row");
}

static std::string joined(const std::vector<std::string>& lines) {
  if (lines.empty()) return "(none)";
  std::string all;
  for (const auto& line : lines) all += (all.empty() ? "" : " | ") + line;
  return all;
}

static std::string directWrites() {
  std::string all;
  for (const auto& write : gWrites) {
    if (write.viaCommand) continue;
    all += (all.empty() ? "" : ", ") + std::to_string(write.from) + " -> " + std::to_string(write.to);
  }
  return all;
}

static void caseNonAdminRefused() {
  gCase = "non_admin_refused";
  resetWorld();
  gSettings.powerSaveTimeoutMinutes = 10;
  pressRow(0);
  EXPECT(directWrites().empty(),
         "the Power Saving row writes powerSaveTimeoutMinutes directly from the OLED task (no admin "
         "check, no [CMD] audit): " + directWrites());
  EXPECT(gSubmitted.size() == 1 && gSubmitted[0] == "powersave 15",
         "the row submits `powersave 15` through the command path, submitted: " + joined(gSubmitted));
  EXPECT(gSettings.powerSaveTimeoutMinutes == 10,
         "a non-admin's press is refused and leaves the timeout at 10, value now: " +
             std::to_string(gSettings.powerSaveTimeoutMinutes));
  EXPECT(gToast == "Admin required",
         "a non-admin's press shows the refusal, toast: '" + gToast + "'");
  EXPECT(gAudit.empty(), "a refused command does not run the handler, audit: " + joined(gAudit));
}

static void caseAdminCyclesLadder() {
  gCase = "admin_cycles_ladder";
  struct Step { uint32_t from; uint32_t to; uint32_t button; };
  // 60 wraps to Off; an off-ladder value (7) goes to the step after Off; X acts like A.
  static const Step steps[] = {
    {10, 15, INPUT_BUTTON_A}, {60, 0, INPUT_BUTTON_A}, {0, 1, INPUT_BUTTON_A}, {7, 1, INPUT_BUTTON_X},
  };
  for (const Step& step : steps) {
    resetWorld();
    gAdmin = true;
    gSettings.powerSaveTimeoutMinutes = step.from;
    pressRow(0, step.button);
    const std::string expected = "powersave " + std::to_string(step.to);
    const std::string at = " (from " + std::to_string(step.from) + ")";
    EXPECT(gSettings.powerSaveTimeoutMinutes == step.to,
           "an admin's press moves the timeout to the next step " + std::to_string(step.to) + at +
               ", value now: " + std::to_string(gSettings.powerSaveTimeoutMinutes));
    EXPECT(gSubmitted.size() == 1 && gSubmitted[0] == expected,
           "the row submits `" + expected + "`" + at + ", submitted: " + joined(gSubmitted));
    EXPECT(gAudit.size() == 1 && gAudit[0] == "[CMD] " + expected,
           "an admin's change leaves a [CMD] audit record" + at + ", audit: " + joined(gAudit));
    EXPECT(directWrites().empty(),
           "the row writes the setting directly from the OLED task" + at + ": " + directWrites());
    EXPECT(gToastCount == 0, "an accepted change shows no toast" + at + ", toast: '" + gToast + "'");
  }
}

static void caseGuestBlocked() {
  gCase = "guest_blocked";
  resetWorld();
  gGuest = true;
  gAdmin = true;
  pressRow(0);
  EXPECT(gSubmitted.empty() && gFireAndForget.empty(),
         "a guest press runs nothing, submitted: " + joined(gSubmitted) + " / " + joined(gFireAndForget));
  EXPECT(gWrites.empty() && gSettings.powerSaveTimeoutMinutes == 10, "a guest press leaves the timeout");
  EXPECT(gToast == "View-only", "a guest press shows View-only, toast: '" + gToast + "'");
}

static void caseOtherRowsUnchanged() {
  gCase = "other_rows_unchanged";
  resetWorld();
  pressRow(1);
  pressRow(2);
  EXPECT(gFireAndForget.size() == 2 && gFireAndForget[0] == "lightsleep 20" &&
             gFireAndForget[1] == "oledmode off",
         "Sleep 20s / Screen Off still run their commands, ran: " + joined(gFireAndForget));
  EXPECT(gSubmitted.empty() && gWrites.empty(), "the other rows do not touch the power-save timeout");
}

int main() {
  caseNonAdminRefused();
  caseAdminCyclesLadder();
  caseGuestBlocked();
  caseOtherRowsUnchanged();
  if (gFailures) {
    std::fprintf(stderr, "%d check(s) failed\n", gFailures);
    return 1;
  }
  std::printf("oled_power_settings harness: all cases passed\n");
  return 0;
}
