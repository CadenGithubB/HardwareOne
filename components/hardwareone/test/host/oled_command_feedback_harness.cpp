// Host harness for test_oled_command_feedback.py. The driver splices production
// code in at the INSERT_* markers: the OLED action helper (OLED_Utils.cpp
// oledRunCommand and oledCommandReplyText, when the tree has them) and real
// callers: the Automations
// toggle/delete actions and their footer toast, the Quick Settings rows and
// the Network screen's HTTP confirm callbacks. Everything else is a double:
// the OLED command executor, the global toast, the Automations list rows and
// the radio / Wi-Fi / Bluetooth / HTTP state.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <type_traits>
#include <vector>

// ---- Arduino String (only what the spliced code uses) ----------------------
class String {
 public:
  String() = default;
  String(const char* text) : value_(text ? text : "") {}
  String(const std::string& text) : value_(text) {}
  const char* c_str() const { return value_.c_str(); }
  size_t length() const { return value_.size(); }

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

// ---- platform --------------------------------------------------------------------
#define ENABLE_G2_GLASSES 1
static uint32_t gNowMs = 1000;
static uint32_t millis() { return gNowMs; }

// ---- OLED command executor doubles ------------------------------------------------
// Every submitted line is recorded; gExecOk/gExecText script the executor's
// verdict and reply (the reply is truncated to the caller's buffer like the real
// executeOLEDCommandWithResult).
static std::vector<std::string> gChecked;       // executeOLEDCommandWithResult lines
static std::vector<std::string> gFireAndForget; // executeOLEDCommand lines (reply dropped)
static bool gExecOk = true;
static std::string gExecText = "OK";
bool executeOLEDCommandWithResult(const String& cmd, char* out, size_t outSize) {
  gChecked.emplace_back(cmd.c_str());
  if (out && outSize) std::snprintf(out, outSize, "%s", gExecText.c_str());
  return gExecOk;
}
void executeOLEDCommand(const String& cmd) { gFireAndForget.emplace_back(cmd.c_str()); }

// ---- global toast (OLED_UI.h) -------------------------------------------------------
enum class OledUIIcon : uint8_t { NONE, INFO, SUCCESS, WARNING, ERROR, QUESTION };
static std::string gToast;
static int gToastCount = 0;
static void oledToastShow(const char* message, uint32_t = 3000, OledUIIcon = OledUIIcon::NONE) {
  gToast = message ? message : "";
  ++gToastCount;
}

// ---- production: the OLED action helper ---------------------------------------------
#if HW1_HAVE_RUN_COMMAND
// INSERT_RUN_COMMAND
#endif

// ---- Automations doubles (rows carry only what the actions read) ----------------------
struct HarnessAutoRow { long id; bool enabled; };
using AutomationSummary = HarnessAutoRow;  // current tree's row type
using AutoListItem = HarnessAutoRow;       // older tree's row type
struct AutomationRenderData {
  HarnessAutoRow items[4];
  int count;
  int selectedIdx;
  bool valid;
  unsigned long lastRefresh;
};
static AutomationRenderData autoRenderData;
static const char* autoActionMsg = nullptr;
static unsigned long autoActionMsgTime = 0;
static bool autoForceRefresh = false;
static const unsigned long AUTO_TOAST_MS = 1500;
static long sPendingDeleteId = 0;
static unsigned long gDirtyUntil = 0;
static void oledMarkDirtyUntil(unsigned long untilMs) { if (untilMs > gDirtyUntil) gDirtyUntil = untilMs; }

// ---- Quick Settings / Network doubles -------------------------------------------------
static char quickStatusMsg[32] = "";
static unsigned long quickStatusExpireMs = 0;
static bool gRadioOn = true;
bool wifiRadioOn() { return gRadioOn; }
struct HarnessWiFi {
  bool connected = true;
  bool isConnected() const { return connected; }
};
static HarnessWiFi WiFi;
static bool gBleServer = false, gG2Client = false;
static bool isBleServerInitialized() { return gBleServer; }
static bool isG2ClientInitialized() { return gG2Client; }
static const int BLE_MODE_SERVER = 1, BLE_MODE_G2_CLIENT = 2;
struct HarnessSettings { int bleMode = BLE_MODE_SERVER; };
static HarnessSettings gSettings;
static int gHttpHandle = 0;
static void* server = nullptr;

// ---- production: callers ------------------------------------------------------------
// INSERT_AUTOMATIONS
// INSERT_QUICK_SETTINGS
// INSERT_NETWORK

// ---- cases ------------------------------------------------------------------------------
static const char* kAdminRefusal =
    "Error: Admin access required for command 'radiopower'. Contact an administrator.";

static void resetWorld() {
  gChecked.clear();
  gFireAndForget.clear();
  gExecOk = true;
  gExecText = "OK";
  gToast.clear();
  gToastCount = 0;
  autoRenderData = AutomationRenderData();
  autoActionMsg = nullptr;
  autoActionMsgTime = 0;
  autoForceRefresh = false;
  sPendingDeleteId = 0;
  gDirtyUntil = 0;
  quickStatusMsg[0] = '\0';
  quickStatusExpireMs = 0;
  gRadioOn = true;
  WiFi.connected = true;
  gBleServer = gG2Client = false;
  gSettings.bleMode = BLE_MODE_SERVER;
  server = nullptr;
  gNowMs += 10000;
}

static void refuse(const char* reply) {
  gExecOk = false;
  gExecText = reply;
}

static std::string ran() {
  std::string all;
  for (const auto& l : gChecked) all += (all.empty() ? "" : ", ") + l;
  for (const auto& l : gFireAndForget) all += (all.empty() ? "" : ", ") + l;
  return all.empty() ? "(none)" : all;
}

static std::string msg(const char* text) { return text ? text : "(none)"; }

#if HW1_HAVE_RUN_COMMAND
static void case_helper_admin_refusal() {
  gCase = "helper_admin_refusal";
  resetWorld();
  refuse(kAdminRefusal);
  EXPECT(!oledRunCommand("radiopower off"), "a refused command reports failure");
  EXPECT(gToast == "Admin required" && gToastCount == 1,
         "an admin denial toasts 'Admin required', toast: '" + gToast + "'");
  EXPECT(gChecked.size() == 1 && gChecked[0] == "radiopower off" && gFireAndForget.empty(),
         "the helper runs the line once through the checked executor, ran: " + ran());

  resetWorld();
  refuse("Error: Super-admin access required for command 'factoryreset'.");
  EXPECT(!oledRunCommand("factoryreset") && gToast == "Admin required",
         "a super-admin denial toasts 'Admin required', toast: '" + gToast + "'");
}

static void case_helper_error_replies() {
  gCase = "helper_error_replies";
  const struct { const char* reply; const char* toast; } kCases[] = {
    {"Error: Not connected to WiFi", "Not connected to WiFi"},
    {"ERROR: sensor busy", "sensor busy"},
    {"[ERROR] Command timed out", "Command timed out"},
    {"Error: first line\nsecond line", "first line"},
    {"Unknown command: frob\nType 'help' for available commands", "Unknown command: frob"},
    {"ERROR", "Failed"},
    {"Error:", "Failed"},
    {"", "Failed"},
  };
  for (const auto& c : kCases) {
    resetWorld();
    refuse(c.reply);
    EXPECT(!oledRunCommand("cmd") && gToast == c.toast && gToastCount == 1,
           std::string("reply '") + c.reply + "' toasts '" + c.toast + "', got '" + gToast + "'");
  }

  // The executor's verdict is double-checked against the reply's error token.
  resetWorld();
  gExecOk = true;
  gExecText = "Error: refused";
  EXPECT(!oledRunCommand("cmd") && gToast == "refused",
         "an 'Error' reply is a failure even when the executor says ok, toast: '" + gToast + "'");

  // A reply longer than the helper's buffer stays bounded (ASan watches).
  resetWorld();
  refuse(("Error: " + std::string(300, 'x')).c_str());
  EXPECT(!oledRunCommand("cmd") && !gToast.empty() && gToast.size() < 64,
         "an over-long reply is bounded to the toast buffer");
}

#if HW1_HAVE_REPLY_TEXT
// The shared reply-to-toast mapping that oledRunCommand and the Add-WiFi flow
// (OLED_Mode_Network.cpp) both use.
static void case_reply_text() {
  gCase = "reply_text";
  resetWorld();
  const struct { bool ok; const char* reply; const char* text; } kCases[] = {
    {true, "OK: Saved network 'HomeNet' with priority 1", "Network saved"},
    {false, "Error: Admin access required for command 'wifiadd'. Contact an administrator.",
     "Admin required"},
    {false, "Error: Super-admin access required for command 'factoryreset'.", "Admin required"},
    {false, "[ERROR] Command timed out", "Command timed out"},
    {false, "ERROR: list full\nsecond line", "list full"},
    {false, "Unknown command: frob", "Unknown command: frob"},
    {false, "ERROR", "Add failed"},
    {false, "", "Add failed"},
  };
  for (const auto& c : kCases) {
    char out[40];
    std::memset(out, '#', sizeof(out));
    oledCommandReplyText(c.ok, c.reply, "Network saved", "Add failed", out, sizeof(out));
    EXPECT(std::string(out) == c.text,
           std::string("ok=") + (c.ok ? "1" : "0") + " reply '" + c.reply + "' gives '" + c.text +
               "', got '" + out + "'");
  }
  char out[40];
  oledCommandReplyText(true, "OK", nullptr, "Failed", out, sizeof(out));
  EXPECT(out[0] == '\0', std::string("success without okText leaves the text empty, got '") + out + "'");
  oledCommandReplyText(false, nullptr, nullptr, "Failed", out, sizeof(out));
  EXPECT(std::string(out) == "Failed", std::string("a missing reply gives failText, got '") + out + "'");

  // A reply longer than the caller's buffer is cut and stays NUL-terminated (ASan watches).
  char small[8];
  std::memset(small, '#', sizeof(small));
  oledCommandReplyText(false, "Error: a long refusal reason", nullptr, "Failed", small, sizeof(small));
  EXPECT(std::string(small) == "a long ", std::string("the text is bounded by outSize, got '") + small + "'");
  EXPECT(gToastCount == 0 && gChecked.empty(), "the mapping neither toasts nor runs a command");
}
#endif

static void case_helper_success() {
  gCase = "helper_success";
  resetWorld();
  gExecText = "OK: Radio powered off";
  EXPECT(oledRunCommand("radiopower off"), "an accepted command reports success");
  EXPECT(gToastCount == 0, "success leaves the toast to the caller's own text, toast: '" + gToast + "'");
}
#endif

static void case_automations_toggle() {
  gCase = "automations_toggle";
  resetWorld();
  autoRenderData.items[0] = {7, true};
  autoRenderData.count = 1;
  refuse(kAdminRefusal);
  autoToggleSelected();
  EXPECT(ran() == "automation disable id=7", "the toggle runs the owner command, ran: " + ran());
  EXPECT(autoActionMsg == nullptr,
         "a refused toggle shows no success text, shown: " + msg(autoActionMsg));
  EXPECT(gToast == "Admin required", "a refused toggle shows the refusal, toast: '" + gToast + "'");
  EXPECT(autoForceRefresh, "the list refreshes after the action either way");

  resetWorld();
  autoRenderData.items[0] = {7, false};
  autoRenderData.count = 1;
  gExecText = "OK: Automation enabled";
  autoToggleSelected();
  EXPECT(msg(autoActionMsg) == "Enabled" && gToastCount == 0,
         "an accepted toggle shows 'Enabled' in the list footer, shown: " + msg(autoActionMsg));
}

static void case_automations_delete() {
  gCase = "automations_delete";
  resetWorld();
  sPendingDeleteId = 9;
  refuse(kAdminRefusal);
  autoDeleteConfirmed(nullptr);
  EXPECT(ran() == "automation delete id=9", "delete runs the owner command, ran: " + ran());
  EXPECT(autoActionMsg == nullptr && gToast == "Admin required",
         "a refused delete shows the refusal, not 'Deleted', shown: " + msg(autoActionMsg) +
             ", toast: '" + gToast + "'");

  resetWorld();
  sPendingDeleteId = 9;
  autoDeleteConfirmed(nullptr);
  EXPECT(msg(autoActionMsg) == "Deleted" && gToastCount == 0 && autoForceRefresh,
         "an accepted delete shows 'Deleted', shown: " + msg(autoActionMsg));
}

static void case_quick_radio_wifi() {
  gCase = "quick_radio_wifi";
  resetWorld();
  refuse(kAdminRefusal);
  toggleQuickRadio();
  EXPECT(ran() == "radiopower off", "the radio row runs radiopower, ran: " + ran());
  EXPECT(quickStatusMsg[0] == '\0' && gToast == "Admin required",
         "a refused radio toggle shows the refusal, not a status, status: '" +
             std::string(quickStatusMsg) + "', toast: '" + gToast + "'");

  resetWorld();
  toggleQuickRadio();
  EXPECT(std::string(quickStatusMsg) == "Radio OFF" && gToastCount == 0,
         "an accepted radio toggle shows 'Radio OFF', status: '" + std::string(quickStatusMsg) + "'");

  resetWorld();
  refuse(kAdminRefusal);
  toggleQuickWiFi();
  EXPECT(ran() == "closewifi" && quickStatusMsg[0] == '\0' && gToast == "Admin required",
         "a refused Wi-Fi toggle shows the refusal, ran: " + ran() + ", status: '" +
             std::string(quickStatusMsg) + "'");

  resetWorld();
  WiFi.connected = false;
  toggleQuickWiFi();
  EXPECT(ran() == "openwifi --best" && std::string(quickStatusMsg) == "WiFi ON",
         "an accepted Wi-Fi connect shows 'WiFi ON', status: '" + std::string(quickStatusMsg) + "'");

  resetWorld();
  gRadioOn = false;
  toggleQuickWiFi();
  EXPECT(ran() == "(none)" && std::string(quickStatusMsg) == "Radio off!",
         "with the radio off the Wi-Fi row refuses locally without a command");
}

static void case_quick_bluetooth_http() {
  gCase = "quick_bluetooth_http";
  resetWorld();
  refuse(kAdminRefusal);
  bluetoothToggleConfirmedQuick(nullptr);
  EXPECT(ran() == "openble" && quickStatusMsg[0] == '\0' && gToast == "Admin required",
         "a refused Bluetooth start shows the refusal, ran: " + ran() + ", status: '" +
             std::string(quickStatusMsg) + "'");

  resetWorld();
  gBleServer = true;
  bluetoothToggleConfirmedQuick(nullptr);
  EXPECT(ran() == "closeble" && std::string(quickStatusMsg) == "Bluetooth OFF",
         "an accepted Bluetooth stop shows 'Bluetooth OFF', status: '" + std::string(quickStatusMsg) + "'");

  resetWorld();
  refuse(kAdminRefusal);
  httpToggleConfirmedQuick(nullptr);
  EXPECT(ran() == "openhttp" && quickStatusMsg[0] == '\0' && gToast == "Admin required",
         "a refused HTTP start shows the refusal, ran: " + ran() + ", status: '" +
             std::string(quickStatusMsg) + "'");

  resetWorld();
  server = &gHttpHandle;
  httpToggleConfirmedQuick(nullptr);
  EXPECT(ran() == "closehttp" && std::string(quickStatusMsg) == "HTTP OFF",
         "an accepted HTTP stop shows 'HTTP OFF', status: '" + std::string(quickStatusMsg) + "'");

  resetWorld();
  WiFi.connected = false;
  httpToggleConfirmedQuick(nullptr);
  EXPECT(ran() == "(none)" && std::string(quickStatusMsg) == "Need WiFi first!",
         "without Wi-Fi the HTTP row refuses locally without a command");
}

static void case_network_http_callbacks() {
  gCase = "network_http_callbacks";
  resetWorld();
  refuse(kAdminRefusal);
  httpStartConfirmedNetwork(nullptr);
  EXPECT(ran() == "openhttp" && gToast == "Admin required",
         "a refused HTTP start from the Network menu shows the refusal, ran: " + ran() +
             ", toast: '" + gToast + "'");
  resetWorld();
  refuse(kAdminRefusal);
  httpStopConfirmedNetwork(nullptr);
  EXPECT(ran() == "closehttp" && gToast == "Admin required",
         "a refused HTTP stop from the Network menu shows the refusal, toast: '" + gToast + "'");
}

int main() {
#if HW1_HAVE_RUN_COMMAND
  case_helper_admin_refusal();
  case_helper_error_replies();
  case_helper_success();
#endif
#if HW1_HAVE_REPLY_TEXT
  case_reply_text();
#endif
  case_automations_toggle();
  case_automations_delete();
  case_quick_radio_wifi();
  case_quick_bluetooth_http();
  case_network_http_callbacks();
  if (gFailures) {
    std::fprintf(stderr, "%d check(s) failed\n", gFailures);
    return 1;
  }
  std::printf("oled_command_feedback: all cases passed\n");
  return 0;
}
