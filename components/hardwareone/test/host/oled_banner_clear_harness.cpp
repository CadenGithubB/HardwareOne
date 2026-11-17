// The OLED feedback queue as shipped (OLED_UI.cpp, inserted by
// test_oled_banner_clear.py): clearing the notification center takes back the
// messages of the cleared events, pending or showing, and leaves the others.
#include "OLED_MessageText.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static bool oledBootModeActive = false;
static uint32_t gNowMs = 1000;
static uint32_t millis() { return gNowMs; }
static int sToastMux = 0;
#define portENTER_CRITICAL(m) do { (void)(m); } while (0)
#define portEXIT_CRITICAL(m) do { (void)(m); } while (0)
static void oledMarkDirty() {}

// INSERT_TYPES

void oledToastShow(const char*, uint32_t = 3000, OledUiIcon = OledUiIcon::NONE);
void oledEventBannerShow(uint32_t, const char*, OledUiIcon, uint32_t = 2500);
OledToast gOledToast{};

// INSERT_PRODUCTION

static unsigned checks = 0, failures = 0;
#define CHECK(cond, what) do { ++checks; if (!(cond)) { ++failures; \
  std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, what); } } while (0)

static std::vector<std::string> gShown;  // every message that went on screen, in order
static std::string gLast;

// Runs the queue's own expiry step and notes a message that newly shows.
static void observe() {
  const bool active = oledToastActive();
  const std::string current = active ? gOledToast.message : "";
  if (active && current != gLast) gShown.push_back(current);
  gLast = current;
}

// The visible message reaches the end of its lifetime; the next one shows.
static void finishCurrent() {
  if (gOledToast.active) gNowMs = gOledToast.expireMs;
  observe();
}

static void reset() {
  oledToastClear();
  gShown.clear();
  gLast.clear();
  gNowMs += 100000;
}

static void showEvent(uint32_t seq) {
  char msg[32];
  std::snprintf(msg, sizeof(msg), "event %u", (unsigned)seq);
  oledEventBannerShow(seq, msg, OledUiIcon::INFO, 2500);
  observe();
}

static std::string visible() { return gOledToast.active ? gOledToast.message : ""; }

int main() {
  // The report: five notifications arrive together, the first shows and four
  // wait. Clearing the notification center ends all five.
  reset();
  for (uint32_t seq = 1; seq <= 5; seq++) showEvent(seq);
  CHECK(gShown.size() == 1 && gShown[0] == "event 1", "the first message shows, the rest wait");
  CHECK(gOledToast.eventSeq == 1, "the showing message knows its event");
  CHECK(sToastQueueCount == 4, "four wait");
  oledEventBannersDismissThrough(5);
  observe();
  CHECK(!gOledToast.active, "the showing message ends");
  CHECK(sToastQueueCount == 0, "the waiting ones are gone");
  finishCurrent();
  finishCurrent();
  CHECK(gShown.size() == 1, "no cleared message shows after the clear");

  // A message that is not from an event (a voice error, a command reply) is
  // not in the notification center, so a clear leaves it.
  reset();
  showEvent(1);
  oledNotificationBannerShow("Voice: unknown", OledUiIcon::WARNING, 2000);
  observe();
  showEvent(2);
  oledEventBannersDismissThrough(2);
  observe();
  CHECK(gShown.size() == 2 && gShown[1] == "Voice: unknown", "a message without an event survives the clear");
  CHECK(gOledToast.eventSeq == 0, "it is not mistaken for an event's message");
  finishCurrent();
  CHECK(gShown.size() == 2 && !gOledToast.active, "the cleared event behind it is gone");

  // Events after the clear still show, in order, behind the kept ones.
  reset();
  for (uint32_t seq = 1; seq <= 5; seq++) showEvent(seq);
  oledEventBannersDismissThrough(3);
  observe();
  CHECK(visible() == "event 4" && gOledToast.eventSeq == 4, "event 1 is cleared and event 4 takes its place");
  showEvent(6);
  finishCurrent();
  finishCurrent();
  finishCurrent();
  CHECK(gShown.size() == 4 && gShown[1] == "event 4" && gShown[2] == "event 5" && gShown[3] == "event 6",
        "the newer events show in order");
  CHECK(!gOledToast.active, "and then the queue is idle");

  // Local feedback is never an event's message, and a clear while nothing
  // shows changes nothing.
  reset();
  oledToastShow("Saved", 3000, OledUiIcon::SUCCESS);
  observe();
  oledEventBannersDismissThrough(100);
  observe();
  CHECK(visible() == "Saved", "local feedback is not taken back");
  reset();
  oledEventBannersDismissThrough(100);
  observe();
  CHECK(!gOledToast.active && gShown.empty(), "an idle clear is a no-op");

  // A full queue still drops the oldest pending, and a clear after that keeps
  // the order of what is left.
  reset();
  for (uint32_t seq = 1; seq <= 20; seq++) showEvent(seq);
  CHECK(sToastQueueCount == kToastQueueCapacity, "the queue is full");
  oledEventBannersDismissThrough(15);
  observe();
  for (int i = 0; i < 8; i++) finishCurrent();
  CHECK(gShown.size() == 6 && gShown[1] == "event 16" && gShown[5] == "event 20",
        "after an overflow and a clear only the newer events show, in order");

  // A repeat is coalesced into the message already there. That message then
  // stands for the newer event too: a clear between the two leaves it.
  reset();
  oledEventBannerShow(5, "Battery low", OledUiIcon::WARNING, 2500);
  oledEventBannerShow(7, "Battery low", OledUiIcon::WARNING, 2500);
  observe();
  CHECK(sToastQueueCount == 0 && gOledToast.eventSeq == 7, "the showing message takes the newer event's seq");
  oledEventBannersDismissThrough(6);
  observe();
  CHECK(visible() == "Battery low", "a repeat posted after the clear keeps its message");
  oledEventBannersDismissThrough(7);
  observe();
  CHECK(!gOledToast.active, "a clear that covers the repeat ends it");
  reset();
  showEvent(1);
  oledEventBannerShow(2, "Door open", OledUiIcon::INFO, 2500);
  oledEventBannerShow(9, "Door open", OledUiIcon::INFO, 2500);
  CHECK(sToastQueueCount == 1 && sToastQueue[0].eventSeq == 9, "a waiting message takes the newer event's seq");

  // A confirmation pauses feedback. A message that takes the cleared one's
  // place during the pause still gets its whole lifetime afterwards.
  reset();
  showEvent(1);
  showEvent(2);
  oledFeedbackSetPaused(true);
  const uint32_t pausedAt = gNowMs;
  gNowMs += 5000;
  oledEventBannersDismissThrough(1);
  CHECK(visible() == "event 2" && gOledToast.expireMs == pausedAt + 2500, "the next message starts at the pause");
  oledFeedbackSetPaused(false);
  CHECK(gOledToast.expireMs == gNowMs + 2500, "and has its whole lifetime after it");

  // During boot no event message is queued at all.
  reset();
  oledBootModeActive = true;
  showEvent(3);
  CHECK(!gOledToast.active && sToastQueueCount == 0, "boot shows no event messages");
  oledBootModeActive = false;

  std::printf("oled banner clear: %u checks, %u failures\n", checks, failures);
  return failures ? 1 : 0;
}
