// test_wifi_saved_ui.py also compiles the Add-WiFi forms: the production OLED
// keyboard flow (OLED_Mode_Network.cpp: manual Add and scan pick) and the G2
// join-a-scanned-AP flow (G2_Page_Network.cpp). Both submit a quoted
// `wifiadd "<ssid>" "<pass>"` line, written by the saved-network owner's
// structured add (wifiAddCommandLine), that CommandArgs splits at every `"`. The
// cases check that a `"` or control character in the typed password or in a
// scanned SSID is refused before any line is built, with a visible message,
// that the owner's reply reaches the OLED toast / G2 lens, and that the OLED
// password keyboards open masked (the SSID one plain). Doubles: the
// OLED keyboard, display session epoch, session-fenced executor and toast; the
// G2 command bridge, text entry and lens. Each mismatch prints
// `FAIL [case] ...`; the program exits non-zero when any case failed.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#define EXT_RAM_BSS_ATTR
template <typename... Args> void quietLog(const char*, Args...) {}
#define DEBUG_G2F(...) quietLog(__VA_ARGS__)

static int gFailures = 0;
static const char* gCase = "";
#define EXPECT(cond, msg)                                                      \
  do {                                                                         \
    if (!(cond)) {                                                             \
      std::fprintf(stderr, "FAIL [%s] %s  (%s, line %d)\n", gCase,             \
                   std::string(msg).c_str(), #cond, __LINE__);                 \
      ++gFailures;                                                             \
    }                                                                          \
  } while (0)

class String : public std::string {
 public:
  using std::string::string;
  String() = default;
  String(const std::string& value) : std::string(value) {}
};

// Printable form of a line for failure messages (TAB/LF/control bytes escaped).
static std::string shown(const std::string& text) {
  std::string out;
  for (unsigned char c : text) {
    char hex[8];
    if (c == '\t') out += "\\t";
    else if (c == '\n') out += "\\n";
    else if (c < 0x20 || c == 0x7F) { std::snprintf(hex, sizeof(hex), "\\x%02X", c); out += hex; }
    else out += static_cast<char>(c);
  }
  return out;
}
static std::string joined(const std::vector<std::string>& lines) {
  if (lines.empty()) return "(none)";
  std::string out;
  for (const std::string& line : lines) out += (out.empty() ? "" : " | ") + shown(line);
  return out;
}

// ---- OLED doubles ----------------------------------------------------------------
using TransportSessionEpoch = uint32_t;
constexpr TransportSessionEpoch kNoTransportSessionEpoch = 0;
enum CommandSource : uint8_t { SOURCE_LOCAL_DISPLAY = 1 };
static TransportSessionEpoch gLiveEpoch = 7;  // the display session's incarnation
static TransportSessionEpoch localDisplayTransportSessionSnapshot(String& user, bool& authed) {
  user = "operator";
  authed = true;
  return gLiveEpoch;
}
static bool transportSessionEpochIsLive(CommandSource, TransportSessionEpoch epoch) {
  return epoch != kNoTransportSessionEpoch && epoch == gLiveEpoch;
}
static void secureClearString(String& text) {
  std::fill(text.begin(), text.end(), '\0');
  text.clear();
}

#define OLED_KEYBOARD_MAX_LENGTH 128
enum class OLEDKeyboardDictationPolicy : uint8_t { ALLOW_PLAINTEXT, DENY };
struct FakeKeyboard {
  bool active = false, completed = false, cancelled = false, masked = false;
  std::string title, text;
};
static FakeKeyboard gKeyboard;
static int gKeyboardOpens = 0;
static void oledKeyboardInit(const char* title, const char*, int, OLEDKeyboardDictationPolicy,
                             bool masked = false) {
  gKeyboard = FakeKeyboard{};
  gKeyboard.active = true;
  gKeyboard.masked = masked;
  gKeyboard.title = title;
  ++gKeyboardOpens;
}
static void oledKeyboardReset() { gKeyboard = FakeKeyboard{}; }
static bool oledKeyboardIsCompleted() { return gKeyboard.completed; }
static bool oledKeyboardIsCancelled() { return gKeyboard.cancelled; }
static const char* oledKeyboardGetText() { return gKeyboard.text.c_str(); }

// Session-fenced OLED executor: records each admitted line and answers from the
// scripted replies in order (default: success "OK"). gReplaceSession models a
// remote login replacing the display session while the command runs: the
// result is fenced (empty, false) like executeOLEDCommandWithResultImpl.
struct Reply { bool ok; std::string text; };
static std::vector<std::string> gOledLines;
static std::vector<Reply> gReplies;
static bool gReplaceSession = false;
static bool executeOLEDCommandWithResultForSession(const String& line, TransportSessionEpoch epoch,
                                                  char* out, size_t outSize) {
  out[0] = '\0';
  if (epoch != gLiveEpoch) return false;
  gOledLines.push_back(line);
  Reply reply{true, "OK"};
  if (!gReplies.empty()) {
    reply = gReplies.front();
    gReplies.erase(gReplies.begin());
  }
  if (gReplaceSession) {
    ++gLiveEpoch;
    return false;
  }
  std::snprintf(out, outSize, "%s", reply.text.c_str());
  return reply.ok;
}

enum class OledUIIcon : uint8_t { NONE };
static std::string gToast;
static int gToastCount = 0;
static void oledToastShow(const char* message, uint32_t = 3000, OledUIIcon = OledUIIcon::NONE) {
  gToast = message ? message : "";
  ++gToastCount;
}

// ---- production: the saved-network owner's field rule and structured add line
// (network/wifi/System_WiFiNetworks.cpp, when present) ----------------------------------
// INSERT_WIFI_FIELD_OWNER

// ---- production: OLED reply-to-toast mapping (OLED_Utils.cpp, when present) ---------
// INSERT_OLED_REPLY_TEXT

// ---- production: OLED Add-WiFi flow ----------------------------------------------
// INSERT_OLED_STATE

// INSERT_OLED_FUNCTIONS

// ---- G2 doubles ----------------------------------------------------------------------
// INSERT_G2_TYPES

static G2HijackPage g2GetHijackPage() { return G2_HIJACK_PAGE_NETWORK; }
static std::vector<std::string> gG2Lines;
static G2HijackCmdCallback gG2Callback = nullptr;
static G2CmdCookie gG2Cookie{};
static bool g2SubmitHijackCommand(const char* line, const G2CmdCookie& cookie,
                                  G2HijackCmdCallback callback, void*) {
  gG2Lines.emplace_back(line);
  gG2Callback = callback;
  gG2Cookie = cookie;
  return true;
}
static int gTextEntries = 0;
static TextEntryCommitFn gCommit = nullptr;
static bool g2BeginTextEntry(const TextEntryConfig& config) {
  ++gTextEntries;
  gCommit = config.onCommit;
  return true;
}
static std::string gLens;
static bool g2ShowText(const char* text) {
  gLens = text ? text : "";
  return true;
}
static bool g2EnqueueRedraw(const G2CmdCookie&, void (*render)(), const char*) {
  render();
  return true;
}
static int gScanResultsShown = 0;
static void showScanResults() { ++gScanResultsShown; }
static void showWiFiMenu() {}

// ---- production: G2 join-a-scanned-AP flow --------------------------------------------
// INSERT_G2_STATE

// INSERT_G2_FUNCTIONS

// ---- OLED drivers ------------------------------------------------------------------------
static void oledBegin(const char* name) {
  gCase = name;
  wifiAddReset();
  gOledLines.clear();
  gReplies.clear();
  gToast.clear();
  gToastCount = 0;
  gKeyboardOpens = 0;
  gReplaceSession = false;
}
// Completes the open keyboard with `text` and lets the mode tick the flow.
static void oledType(const char* text) {
  gKeyboard.text = text;
  gKeyboard.completed = true;
  bool handled = false;
  (void)wifiAddKeyboardTick(&handled);
}
static void oledManualAdd(const char* ssid, const char* pass) {
  startWifiAddSSID();
  oledType(ssid);
  if (gKeyboard.active) oledType(pass);
}
static void oledScanPick(const char* ssid, const char* pass) {
  startWifiAddFromScan(ssid);
  if (gKeyboard.active) oledType(pass);
}
static bool oledRan(const std::vector<std::string>& lines) { return gOledLines == lines; }

// ---- G2 drivers ----------------------------------------------------------------------------
static void g2Begin(const char* name) {
  gCase = name;
  gG2Lines.clear();
  gG2Callback = nullptr;
  gTextEntries = 0;
  gCommit = nullptr;
  gLens.clear();
  gScanResultsShown = 0;
  std::memset(gScanCache, 0, sizeof(gScanCache));
  gScanCacheCount = 0;
  gNetSub = NET_SUB_WIFI_SCAN;
}
static void g2Scanned(const char* ssid, bool secured) {
  CachedAp& ap = gScanCache[gScanCacheCount++];
  std::snprintf(ap.ssid, sizeof(ap.ssid), "%s", ssid);
  ap.rssi = -40;
  ap.secured = secured;
}
static bool lensRefusesQuote() {
  return gLens.find("double-quote") != std::string::npos;
}

// ---- cases ------------------------------------------------------------------------------------
static void oledPasswordQuote() {
  oledBegin("oled_password_quote");
  oledManualAdd("HomeNet", "pa\"ss");
  EXPECT(gOledLines.empty(),
         "a password with a double-quote builds no wifiadd line, ran: " + joined(gOledLines));
  EXPECT(gToast == "Quote not allowed", "the refusal is toasted, toast: '" + gToast + "'");
  EXPECT(sWifiAddStep == WIFI_ADD_NONE && sWifiAddPass.empty() && !sWifiKeyboardActive,
         "the flow ends and the typed password is wiped");
}

static void oledScanSsidQuote() {
  oledBegin("oled_scan_ssid_quote");
  oledScanPick("x\" \"y", "secret12");
  EXPECT(gKeyboardOpens == 0, "a scanned SSID with a double-quote opens no password keyboard");
  EXPECT(gOledLines.empty(),
         "a scanned SSID with a double-quote builds no wifiadd line, ran: " + joined(gOledLines));
  EXPECT(gToast == "Quote not allowed", "the refusal is toasted, toast: '" + gToast + "'");
}

static void oledControlCharacters() {
  oledBegin("oled_ssid_control");
  oledManualAdd("Lab\tNet", "secret12");
  EXPECT(gKeyboardOpens == 1, "a typed SSID with a control character asks for no password");
  EXPECT(gOledLines.empty(),
         "a typed SSID with a control character builds no line, ran: " + joined(gOledLines));
  EXPECT(gToast == "Invalid character", "typed SSID refusal toasted, toast: '" + gToast + "'");

  oledBegin("oled_ssid_control");
  oledScanPick("Cafe\nNet", "secret12");
  EXPECT(gOledLines.empty(),
         "a scanned SSID with a line feed builds no line, ran: " + joined(gOledLines));
  EXPECT(gToast == "Invalid character", "scanned SSID refusal toasted, toast: '" + gToast + "'");

  oledBegin("oled_ssid_control");
  oledManualAdd("HomeNet", "pa\x01ss");
  EXPECT(gOledLines.empty(),
         "a password with a control byte builds no line, ran: " + joined(gOledLines));
  EXPECT(gToast == "Invalid character", "password refusal toasted, toast: '" + gToast + "'");
}

static void oledRefusalToast() {
  oledBegin("oled_refusal_toast");
  gReplies = {{false, "Error: Admin access required for command 'wifiadd'. Contact an administrator."}};
  oledManualAdd("HomeNet", "secret12");
  EXPECT(oledRan({"wifiadd \"HomeNet\" \"secret12\""}), "ran: " + joined(gOledLines));
  EXPECT(gToast == "Admin required",
         "an admin refusal of wifiadd is toasted, toast: '" + gToast + "'");

  oledBegin("oled_refusal_toast");
  gReplies = {{false, "Error: saved-network list is full (8) - remove one with wifirm first"}};
  oledManualAdd("HomeNet", "secret12");
  EXPECT(gToast.rfind("saved-network list is full (8)", 0) == 0,
         "the owner's refusal is toasted without its error token, toast: '" + gToast + "'");

  oledBegin("oled_refusal_toast");
  gReplies = {{false, "Error: Admin access required for command 'wifiadd'. Contact an administrator."}};
  oledScanPick("HomeNet", "secret12");
  EXPECT(oledRan({"wifiadd \"HomeNet\" \"secret12\""}),
         "a refused add never connects, ran: " + joined(gOledLines));
  EXPECT(gToast == "Admin required", "scan-pick refusal toasted, toast: '" + gToast + "'");

  // The same mapping as every other OLED action (OLED_Utils oledCommandReplyText).
  oledBegin("oled_refusal_toast");
  gReplies = {{false, "Error: Super-admin access required for command 'wifiadd'."}};
  oledManualAdd("HomeNet", "secret12");
  EXPECT(gToast == "Admin required",
         "a super-admin refusal is toasted as 'Admin required', toast: '" + gToast + "'");

  oledBegin("oled_refusal_toast");
  gReplies = {{false, "[ERROR] Command timed out"}};
  oledManualAdd("HomeNet", "secret12");
  EXPECT(gToast == "Command timed out",
         "an [ERROR] reply is toasted without its token, toast: '" + gToast + "'");

  oledBegin("oled_refusal_toast");
  gReplies = {{true, "OK: Saved network 'HomeNet' with priority 1"},
              {false, "[ERROR] No saved network in range"}};
  oledScanPick("HomeNet", "secret12");
  EXPECT(gToast == "No saved network in range",
         "a refused connect is toasted without its token, toast: '" + gToast + "'");
}

static void oledSuccessToast() {
  oledBegin("oled_success_toast");
  gReplies = {{true, "OK: Saved network 'HomeNet' with priority 1"}};
  oledManualAdd("HomeNet", "secret12");
  EXPECT(oledRan({"wifiadd \"HomeNet\" \"secret12\""}), "ran: " + joined(gOledLines));
  EXPECT(gToast == "Network saved", "a saved network is toasted, toast: '" + gToast + "'");
  EXPECT(sWifiAddStep == WIFI_ADD_NONE && sWifiAddPass.empty(), "the flow resets after the add");

  oledBegin("oled_success_toast");
  gReplies = {{true, "OK: Saved network 'HomeNet' with priority 1"},
              {true, "OK: [WiFi] Connected successfully"}};
  oledScanPick("HomeNet", "secret12");
  EXPECT(oledRan({"wifiadd \"HomeNet\" \"secret12\"", "openwifi --best"}),
         "a scan pick saves then connects, ran: " + joined(gOledLines));
  EXPECT(gToast == "Connected", "the connect result is toasted, toast: '" + gToast + "'");

  oledBegin("oled_success_toast");
  gReplies = {{true, "OK: Saved network 'HomeNet' with priority 1"}, {false, "ERROR"}};
  oledScanPick("HomeNet", "secret12");
  EXPECT(gToast == "Connect failed", "a failed connect is toasted, toast: '" + gToast + "'");
}

// The password keyboards are secret fields: the keyboard draws them masked.
// The SSID stays plain.
static void oledPasswordMasked() {
  oledBegin("oled_password_masked");
  startWifiAddSSID();
  EXPECT(gKeyboard.active && !gKeyboard.masked, "the SSID keyboard opens plain");
  oledType("HomeNet");
  EXPECT(gKeyboard.active && gKeyboard.title == "Enter Password:" && gKeyboard.masked,
         "the manual-add password keyboard opens masked");

  oledBegin("oled_password_masked");
  startWifiAddFromScan("HomeNet");
  EXPECT(gKeyboard.active && gKeyboard.masked, "the scan-pick password keyboard opens masked");
}

static void oledReplacedSession() {
  oledBegin("oled_replaced_session");
  gReplaceSession = true;
  oledManualAdd("HomeNet", "secret12");
  EXPECT(gToastCount == 0,
         "a replacement display session sees no toast for the old add, toast: '" + gToast + "'");
  gReplaceSession = false;
}

static void g2ScanSsidQuote() {
  g2Begin("g2_scan_ssid_quote");
  g2Scanned("x\" \"y", true);
  g2Scanned("a\"b", false);
  handleWiFiScanTap(1);
  EXPECT(gTextEntries == 0, "a scanned SSID with a double-quote opens no keyboard");
  EXPECT(lensRefusesQuote(), "the refusal is shown on the lens, lens: '" + shown(gLens) + "'");
  gLens.clear();
  handleWiFiScanTap(2);
  EXPECT(gG2Lines.empty(), "an open AP with a double-quote submits nothing, ran: " + joined(gG2Lines));
  EXPECT(lensRefusesQuote(), "the open-AP refusal is shown, lens: '" + shown(gLens) + "'");
}

static void g2ScanSsidControl() {
  g2Begin("g2_scan_ssid_control");
  g2Scanned("Lab\tNet", true);
  g2Scanned("Cafe\nNet", false);
  handleWiFiScanTap(1);
  EXPECT(gTextEntries == 0, "a scanned SSID with a TAB opens no password keyboard");
  EXPECT(lensRefusesQuote(), "the refusal is shown on the lens, lens: '" + shown(gLens) + "'");
  gLens.clear();
  handleWiFiScanTap(2);
  EXPECT(gG2Lines.empty(),
         "an open AP with a line feed in its SSID submits nothing, ran: " + joined(gG2Lines));
  EXPECT(lensRefusesQuote(), "the open-AP refusal is shown, lens: '" + shown(gLens) + "'");
}

static void g2PasswordQuote() {
  g2Begin("g2_password_quote");
  g2Scanned("HomeNet", true);
  handleWiFiScanTap(1);
  EXPECT(gTextEntries == 1 && gCommit, "a secured AP opens the password keyboard");
  if (gCommit) gCommit("pa\"ss");
  EXPECT(gG2Lines.empty(),
         "a committed password with a double-quote builds no line, ran: " + joined(gG2Lines));
  EXPECT(lensRefusesQuote(), "the refusal is shown on the lens, lens: '" + shown(gLens) + "'");
}

static void g2ReplyShown() {
  g2Begin("g2_reply_shown");
  g2Scanned("HomeNet", true);
  g2Scanned("Guest", false);
  g2Scanned("", true);
  handleWiFiScanTap(1);
  if (gCommit) gCommit("secret12");
  EXPECT(gG2Lines == std::vector<std::string>{"wifiadd \"HomeNet\" \"secret12\" 1 0"},
         "a valid password submits the quoted line, ran: " + joined(gG2Lines));
  const char* denial = "Error: Admin access required for command 'wifiadd'. Contact an administrator.";
  if (gG2Callback) gG2Callback(false, denial, gG2Cookie, nullptr);
  EXPECT(gLens == denial, "the owner's reply is shown on the lens, lens: '" + gLens + "'");

  gG2Lines.clear();
  handleWiFiScanTap(2);
  EXPECT(gG2Lines == std::vector<std::string>{"wifiadd \"Guest\" \"\" 1 0"},
         "an open AP saves with a quoted empty password, ran: " + joined(gG2Lines));

  gG2Lines.clear();
  gLens.clear();
  const int entries = gTextEntries;
  handleWiFiScanTap(3);
  EXPECT(gG2Lines.empty() && gTextEntries == entries && gLens.empty(),
         "a hidden AP stays ignored");
}

int main() {
  oledPasswordQuote();
  oledScanSsidQuote();
  oledControlCharacters();
  oledRefusalToast();
  oledSuccessToast();
  oledPasswordMasked();
  oledReplacedSession();
  g2ScanSsidQuote();
  g2ScanSsidControl();
  g2PasswordQuote();
  g2ReplyShown();
  if (gFailures) {
    std::fprintf(stderr, "Add-WiFi forms: %d check(s) failed\n", gFailures);
    return 1;
  }
  std::puts("Add-WiFi forms: OLED and G2 refusal/reply cases passed");
  return 0;
}
