// The OLED banner queue as shipped (OLED_UI.cpp, inserted by
// test_oled_banner_clear.py): clearing the notification center takes back the
// banners of the cleared events, queued or showing, and leaves the others.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#define EXT_RAM_BSS_ATTR
static bool oledBootModeActive = false;
static uint32_t gNowMs = 1000;
static uint32_t millis() { return gNowMs; }

// INSERT_TYPES

OledPairingRibbon gOledPairingRibbon;
static std::vector<std::string> gShown;  // every banner that went on screen, in order

// The display side of the ribbon: what it shows, nothing it draws.
void oledPairingRibbonShow(const char* message, PairingRibbonIcon icon, uint32_t visibleMs, bool blink) {
  gOledPairingRibbon.eventSeq = 0;
  strncpy(gOledPairingRibbon.message, message, sizeof(gOledPairingRibbon.message) - 1);
  gOledPairingRibbon.message[sizeof(gOledPairingRibbon.message) - 1] = '\0';
  gOledPairingRibbon.icon = icon;
  gOledPairingRibbon.visibleDurationMs = visibleMs;
  gOledPairingRibbon.iconBlink = blink;
  gOledPairingRibbon.state = PairingRibbonState::UNFURLING;
  gOledPairingRibbon.stateStartMs = millis();
  gShown.push_back(message);
}

// INSERT_PRODUCTION

static unsigned checks = 0, failures = 0;
#define CHECK(cond, what) do { ++checks; if (!(cond)) { ++failures; \
  std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, what); } } while (0)

// The ribbon finishes its slide-out and the pump puts up the next banner.
static void finishCurrent() {
  gOledPairingRibbon.state = PairingRibbonState::HIDDEN;
  oledBannerQueuePump();
}

static void reset() {
  memset(&gOledPairingRibbon, 0, sizeof(gOledPairingRibbon));
  gOledPairingRibbon.state = PairingRibbonState::HIDDEN;
  oledEventBannersDismissThrough(UINT32_MAX);
  gOledPairingRibbon.state = PairingRibbonState::HIDDEN;
  gShown.clear();
}

static void showEvent(uint32_t seq) {
  char msg[32];
  std::snprintf(msg, sizeof(msg), "event %u", (unsigned)seq);
  oledEventBannerShow(seq, msg, PairingRibbonIcon::INFO_ICON, 2500, false);
}

int main() {
  // The report: five notifications arrive together, the first shows and four
  // wait. Clearing the notification center ends all five.
  reset();
  for (uint32_t seq = 1; seq <= 5; seq++) showEvent(seq);
  CHECK(gShown.size() == 1 && gShown[0] == "event 1", "the first banner shows, the rest wait");
  CHECK(gOledPairingRibbon.eventSeq == 1, "the showing banner knows its event");
  oledEventBannersDismissThrough(5);
  CHECK(gOledPairingRibbon.state == PairingRibbonState::SHRINKING, "the showing banner slides out");
  finishCurrent();
  finishCurrent();
  CHECK(gShown.size() == 1, "no cleared banner comes down after the clear");
  CHECK(gOledPairingRibbon.state == PairingRibbonState::HIDDEN, "the ribbon stays idle");

  // A banner that is not from an event (a voice error, a remote command) is
  // not in the notification center, so a clear leaves it.
  reset();
  showEvent(1);
  oledNotificationBannerShow("Voice: unknown", PairingRibbonIcon::ERROR_ICON, 2000, false);
  showEvent(2);
  oledEventBannersDismissThrough(2);
  finishCurrent();
  CHECK(gShown.size() == 2 && gShown[1] == "Voice: unknown", "a banner without an event survives the clear");
  CHECK(gOledPairingRibbon.eventSeq == 0, "it is not mistaken for an event banner");
  finishCurrent();
  CHECK(gShown.size() == 2, "the cleared event behind it is gone");

  // Events after the clear still show, in order, behind the kept ones.
  reset();
  for (uint32_t seq = 1; seq <= 5; seq++) showEvent(seq);
  oledEventBannersDismissThrough(3);
  CHECK(gOledPairingRibbon.state == PairingRibbonState::SHRINKING, "event 1 is cleared");
  showEvent(6);
  finishCurrent();
  finishCurrent();
  finishCurrent();
  CHECK(gShown.size() == 4 && gShown[1] == "event 4" && gShown[2] == "event 5" && gShown[3] == "event 6",
        "the newer events show in order");

  // A pairing ribbon put up directly is never an event banner, and a clear
  // while nothing shows changes nothing.
  reset();
  oledPairingRibbonShow("Paired", PairingRibbonIcon::LINK, 3000, true);
  oledEventBannersDismissThrough(100);
  CHECK(gOledPairingRibbon.state == PairingRibbonState::UNFURLING, "a pairing ribbon is not taken back");
  reset();
  oledEventBannersDismissThrough(100);
  CHECK(gOledPairingRibbon.state == PairingRibbonState::HIDDEN && gShown.empty(), "an idle clear is a no-op");

  // A full queue still drops the oldest pending, and a clear after that keeps
  // the order of what is left.
  reset();
  showEvent(1);
  for (uint32_t seq = 2; seq <= 20; seq++) showEvent(seq);
  oledEventBannersDismissThrough(15);
  for (int i = 0; i < 8; i++) finishCurrent();
  CHECK(gShown.size() == 6 && gShown[1] == "event 16" && gShown[5] == "event 20",
        "after an overflow and a clear only the newer events show, in order");

  std::printf("oled banner clear: %u checks, %u failures\n", checks, failures);
  return failures ? 1 : 0;
}
