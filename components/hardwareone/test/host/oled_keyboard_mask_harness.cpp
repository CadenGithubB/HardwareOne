// test_dictation_ui.py also compiles this unit: the shipping OLED virtual
// keyboard (OLED_Utils.cpp: the character tables, getCharAt, oledKeyboardInit,
// oledKeyboardReset, the preview and oledKeyboardDisplay, with the real
// OLEDKeyboardState from OLED_Utils.h) and the Set Pattern flow's keyboard
// openers (OLED_Mode_SetPattern.cpp). A recording display keeps the text each
// cursor row printed. A masked (secret) field must never print its text, or any
// 3-character piece of it, on any keyboard page; its preview shows one '*' per
// character. A plain field keeps its text preview. Each mismatch prints
// `FAIL [case] ...`; the program exits non-zero when any check failed.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>

static int gFailures = 0;
static int gChecks = 0;
static std::string gCase;
#define EXPECT(cond, msg)                                                      \
  do {                                                                         \
    ++gChecks;                                                                 \
    if (!(cond)) {                                                             \
      std::fprintf(stderr, "FAIL [%s] %s  (%s, line %d)\n", gCase.c_str(),    \
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
using std::min;

#define DEBUG_DISPLAYF(...) ((void)0)
static uint32_t gNowMs = 0;  // (millis() / 500) % 2 == 0: the blinking cursor is drawn
static uint32_t millis() { return gNowMs; }
static constexpr int DISPLAY_COLOR_WHITE = 1, DISPLAY_COLOR_BLACK = 0;
enum CommandSource { SOURCE_INTERNAL, SOURCE_G2_GLASSES, SOURCE_LOCAL_DISPLAY };

// Recording panel: every print lands in the row of the last setCursor().
class Adafruit_SSD1306 {
 public:
  std::map<int, std::string> rows;
  void clear() { rows.clear(); row = 0; }
  void setTextSize(int) {}
  void setTextColor(int) {}
  void setTextColor(int, int) {}
  void setCursor(int, int y) { row = y; }
  void drawRect(int, int, int, int, int) {}
  void fillRect(int, int, int, int, int) {}
  void fillCircle(int, int, int, int) {}
  void print(const char* text) { rows[row] += text; }
  void print(const String& text) { rows[row] += text; }
  void print(char c) { rows[row] += c; }
  std::string row_at(int y) const {
    auto it = rows.find(y);
    return it == rows.end() ? std::string() : it->second;
  }

 private:
  int row = 0;
};

// INSERT_TYPES

static void dictationCancel() {}
static void secureClearString(String& text) {
  std::fill(text.begin(), text.end(), '\0');
  text.clear();
}
static DictationSnapshot dictationSnapshotNow() {
  DictationSnapshot snap{};
  snap.state = DictationState::IDLE;
  snap.ownerSource = SOURCE_LOCAL_DISPLAY;
  snap.sourceName = "PDM";
  return snap;
}

// INSERT_KEYBOARD

// INSERT_SET_PATTERN

// Opens a field through the production oledKeyboardInit. A tree whose keyboard
// takes the mask flag gets it; a tree without one cannot mask, so the field
// opens plain and the masked cases fail on what the panel shows.
template <typename... Args>
static auto openFieldWith(int, Args... args) -> decltype(oledKeyboardInit(args...)) {
  oledKeyboardInit(args...);
}
template <typename Policy>
static void openFieldWith(long, const char* title, const char* text, int maxLength,
                          Policy policy, bool) {
  oledKeyboardInit(title, text, maxLength, policy);
}
static void openField(const char* text, bool masked) {
  openFieldWith(0, "Enter Password:", text, OLED_KEYBOARD_MAX_LENGTH,
                OLEDKeyboardDictationPolicy::DENY, masked);
}
static void typeText(const char* text) {
  std::snprintf(gOledKeyboardState.text, sizeof(gOledKeyboardState.text), "%s", text);
  gOledKeyboardState.textLength = static_cast<int>(std::strlen(gOledKeyboardState.text));
}

static Adafruit_SSD1306 gPanel;
static constexpr int kPreviewRow = 11;  // keyboardStartY + 11: the text preview box
static constexpr int kTitleRow = 0;     // the suggestions page shows its preview here

enum class Page { LOWER, UPPER, NUMBERS, SYMBOLS, PATTERN, MIC, SUGGESTIONS };
static const char* pageName(Page page) {
  switch (page) {
    case Page::LOWER: return "lowercase";
    case Page::UPPER: return "uppercase";
    case Page::NUMBERS: return "numbers";
    case Page::SYMBOLS: return "symbols";
    case Page::PATTERN: return "pattern";
    case Page::MIC: return "mic";
    case Page::SUGGESTIONS: return "suggestions";
  }
  return "?";
}
static const Page kPages[] = {Page::LOWER, Page::UPPER, Page::NUMBERS, Page::SYMBOLS,
                              Page::PATTERN, Page::MIC, Page::SUGGESTIONS};
static const char* const kSuggestion = "sugg";

static void renderPage(Page page) {
  switch (page) {
    case Page::LOWER: gOledKeyboardState.mode = KEYBOARD_MODE_LOWERCASE; break;
    case Page::UPPER: gOledKeyboardState.mode = KEYBOARD_MODE_UPPERCASE; break;
    case Page::NUMBERS: gOledKeyboardState.mode = KEYBOARD_MODE_NUMBERS; break;
    case Page::SYMBOLS: gOledKeyboardState.mode = KEYBOARD_MODE_SYMBOLS; break;
    case Page::PATTERN: gOledKeyboardState.mode = KEYBOARD_MODE_PATTERN; break;
    case Page::MIC: gOledKeyboardState.mode = KEYBOARD_MODE_MIC; break;
    case Page::SUGGESTIONS:
      gOledKeyboardState.mode = KEYBOARD_MODE_LOWERCASE;
      gOledKeyboardState.showingSuggestions = true;
      gOledKeyboardState.suggestions[0] = kSuggestion;
      gOledKeyboardState.suggestionCount = 1;
      gOledKeyboardState.selectedSuggestion = 0;
      break;
  }
  gPanel.clear();
  oledKeyboardDisplay(&gPanel);
}

// The first printed row that shows `secret` or any 3-character piece of it.
static std::string leakedRow(const std::string& secret) {
  for (const auto& entry : gPanel.rows) {
    for (size_t i = 0; i + 3 <= secret.size(); ++i) {
      if (entry.second.find(secret.substr(i, 3)) != std::string::npos) {
        return "row " + std::to_string(entry.first) + " '" + entry.second + "'";
      }
    }
  }
  return "";
}

static std::string expectedPreview(Page page, const std::string& text, bool masked) {
  if (page == Page::SUGGESTIONS) {
    const size_t shown = std::min<size_t>(text.size(), 8);
    return "Suggestions:" + (masked ? std::string(shown, '*') : text.substr(0, shown));
  }
  const size_t shown = std::min<size_t>(text.size(), 20);
  return (masked ? std::string(shown, '*') : text.substr(text.size() - shown)) + "_";
}
static std::string previewRow(Page page) {
  return gPanel.row_at(page == Page::SUGGESTIONS ? kTitleRow : kPreviewRow);
}

// A masked field never shows its text on any page; the preview is '*' per
// character (last 20, first 8 on the suggestions page).
static void maskedFieldHidesText() {
  const char* const secrets[] = {"Kq7mZp3Rv9", "Kq7mZp3Rv9Wt4Ns8Bx2Hc6", "^^>v<<v>"};
  for (const char* secret : secrets) {
    for (Page page : kPages) {
      gCase = std::string("masked_") + pageName(page);
      openField(secret, true);
      renderPage(page);
      const std::string leak = leakedRow(secret);
      EXPECT(leak.empty(), std::string("masked field on the ") + pageName(page) +
                               " page shows the secret '" + secret + "' in " + leak);
      EXPECT(previewRow(page) == expectedPreview(page, secret, true),
             std::string("masked preview is '") + previewRow(page) + "', expected '" +
                 expectedPreview(page, secret, true) + "'");
      EXPECT(std::string(gOledKeyboardState.text) == secret,
             "masking is display-only: the field still holds the typed text");
      oledKeyboardReset();
    }
  }
}

// A plain field keeps the preview it always had.
static void plainFieldShowsText() {
  const char* const texts[] = {"Kq7mZp3Rv9", "Kq7mZp3Rv9Wt4Ns8Bx2Hc6"};
  for (const char* text : texts) {
    for (Page page : kPages) {
      gCase = std::string("plain_") + pageName(page);
      openField(text, false);
      renderPage(page);
      EXPECT(previewRow(page) == expectedPreview(page, text, false),
             std::string("plain preview is '") + previewRow(page) + "', expected '" +
                 expectedPreview(page, text, false) + "'");
      oledKeyboardReset();
    }
  }
}

// A plain field opened after a masked one shows its text again.
static void plainAfterMasked() {
  gCase = "plain_after_masked";
  openField("Kq7mZp3Rv9", true);
  oledKeyboardReset();
  EXPECT(gOledKeyboardState.text[0] == '\0', "reset clears the field");
  oledKeyboardInit("Enter Username:", "alice42", OLED_KEYBOARD_MAX_LENGTH,
                   OLEDKeyboardDictationPolicy::ALLOW_PLAINTEXT);
  renderPage(Page::LOWER);
  EXPECT(gPanel.row_at(kPreviewRow) == "alice42_",
         "a plain field after a masked one shows '" + gPanel.row_at(kPreviewRow) + "'");
  oledKeyboardReset();
}

// The Set Pattern flow's openers (current password, current/new/confirm
// pattern) open masked keyboards.
static void setPatternKeyboardsMasked() {
  gCase = "set_pattern_text";
  startTextKeyboard("Enter password:");
  typeText("Kq7mZp3Rv9");
  renderPage(Page::LOWER);
  std::string leak = leakedRow("Kq7mZp3Rv9");
  EXPECT(leak.empty(), "Set Pattern's password keyboard shows the password in " + leak);
  EXPECT(gPanel.row_at(kPreviewRow) == "**********_",
         "Set Pattern password preview is '" + gPanel.row_at(kPreviewRow) + "'");
  oledKeyboardReset();

  gCase = "set_pattern_pattern";
  startPatternKeyboard("Current pattern:");
  EXPECT(gOledKeyboardState.mode == KEYBOARD_MODE_PATTERN, "the pattern keyboard opens on the pattern page");
  typeText("^^>v<<v>");
  gPanel.clear();
  oledKeyboardDisplay(&gPanel);
  leak = leakedRow("^^>v<<v>");
  EXPECT(leak.empty(), "Set Pattern's pattern keyboard shows the pattern in " + leak);
  EXPECT(gPanel.row_at(kPreviewRow) == "********_",
         "Set Pattern pattern preview is '" + gPanel.row_at(kPreviewRow) + "'");
  bool counted = false;
  for (const auto& entry : gPanel.rows) counted = counted || entry.second == "8 moves";
  EXPECT(counted, "the pattern page still counts the moves");
  oledKeyboardReset();
  sKeyboardActive = false;
}

int main() {
  maskedFieldHidesText();
  plainFieldShowsText();
  plainAfterMasked();
  setPatternKeyboardsMasked();
  if (gFailures) {
    std::fprintf(stderr, "OLED keyboard masking: %d of %d check(s) failed\n", gFailures, gChecks);
    return 1;
  }
  std::printf("OLED keyboard masking: %d checks passed\n", gChecks);
  return 0;
}
