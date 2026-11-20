// Exercise the production transport with failed SDK calls and no hardware.
// c++ -std=c++17 -Isdmmc_stubs test_sdmmc_lifecycle.cpp -o /tmp/sdmmc-test
#define SYSTEM_BUILDCONFIG_H
#define CONFIG_IDF_TARGET_ESP32P4 1
#ifdef TEST_WAVESHARE
#define HW_BOARD_WAVESHARE_P4_TOUCH 1
#define HW_BOARD_P4X_EYE 0
#else
#define HW_BOARD_P4X_EYE 1
#define HW_BOARD_WAVESHARE_P4_TOUCH 0
#endif
#define ENABLE_SDMMC_CARD 1
#define ENABLE_SD_CARD 1
#ifdef TEST_WAVESHARE
#include "../../System_Board_WaveshareP4.h"
#else
#include "../../System_Board_P4X_EYE.h"
#endif
#include "../../HAL_SDCard.cpp"
#include "../../System_SdWatchCore.h"  // UP-06: the pure policy, run against this driver
#include <cassert>
#include <vector>

namespace {
bool inserted = true;
// The detect pin's input. gpio_config enables it; the SDMMC driver disables
// it again whenever it releases the slot (sdmmc_host_deinit_slot resets every
// pin of the slot, the card-detect pin included), and a pin with its input
// disabled reads 0, the level that means "card in".
bool detectArmed = false;
int powerLevel = 1, ldoCount = 0, mounts = 0, formats = 0;
esp_err_t mountResult = ESP_OK, ldoResult = ESP_OK, formatResult = ESP_OK;
esp_err_t unmountResult = ESP_OK;
FRESULT fatRemountResult = FR_OK;
FATFS volume;
sdmmc_card_t card;
sdmmc_host_t capturedHost;
bool capturedFormat = false;
std::vector<std::string> calls;
}

void delay(unsigned) {}
const char* esp_err_to_name(esp_err_t e) { return e == ESP_OK ? "OK" : "error"; }
esp_err_t gpio_set_level(gpio_num_t pin, int level) {
  assert(pin == SD_MMC_POWER_PIN);
  powerLevel = level;
  calls.push_back(level ? "power-off" : "power-on");
  return ESP_OK;
}
esp_err_t gpio_config(const gpio_config_t* c) {
#if SD_MMC_DETECT_PIN >= 0
  if (c->pin_bit_mask == (1ULL << SD_MMC_DETECT_PIN)) {
    assert(c->mode == GPIO_MODE_INPUT && c->pull_up_en == GPIO_PULLUP_ENABLE);
    detectArmed = true;
  }
#else
  (void)c;
#endif
  return ESP_OK;
}
int gpio_get_level(gpio_num_t pin) {
  assert(SD_MMC_DETECT_PIN >= 0 && pin == SD_MMC_DETECT_PIN);
  if (!detectArmed) return 0;  // input disabled: reads "card in" whatever the switch does
  return inserted ? 0 : 1;
}
esp_err_t sd_pwr_ctrl_new_on_chip_ldo(const sd_pwr_ctrl_ldo_config_t* c, sd_pwr_ctrl_handle_t* out) {
  assert(c->ldo_chan_id == 4);
  if (ldoResult != ESP_OK) return ldoResult;
  ++ldoCount;
  *out = &card;
  return ESP_OK;
}
esp_err_t sd_pwr_ctrl_del_on_chip_ldo(sd_pwr_ctrl_handle_t p) {
  assert(p == &card && powerLevel == 1);
  --ldoCount;
  calls.push_back("release-ldo");
  return ESP_OK;
}
esp_err_t sdmmc_host_deinit_slot(int slot) {
  assert(slot == 0);  // slot 1 belongs to the radio, including mount failures
  calls.push_back("release-slot");
  detectArmed = false;  // gpio_reset_pin on the slot's card-detect pin
  return ESP_OK;
}
esp_err_t esp_vfs_fat_sdmmc_mount(const char* path, const sdmmc_host_t* h,
    const sdmmc_slot_config_t* s, const esp_vfs_fat_sdmmc_mount_config_t* c,
    sdmmc_card_t** out) {
  ++mounts;
  assert(std::string(path) == "/sd" && powerLevel == 0 && ldoCount == 1);
  assert(h->slot == 0 && h->max_freq_khz == 40000);
  assert(h->flags & SDMMC_HOST_FLAG_DEINIT_ARG);
  assert(h->deinit_p == sdmmc_host_deinit_slot && h->pwr_ctrl_handle == &card);
  assert(s->width == 4 && s->clk == 43 && s->cmd == 44);
  assert(s->d0 == 39 && s->d1 == 40 && s->d2 == 41 && s->d3 == 42);
  assert(s->cd == SD_MMC_DETECT_PIN && s->d4 == -1 && s->d5 == -1 && s->d6 == -1 && s->d7 == -1);
  capturedHost = *h;
  capturedFormat = c->format_if_mount_failed;
  if (mountResult == ESP_OK) *out = &card;
  else h->deinit_p(h->slot); // FatFs failure unwind must leave radio intact
  return mountResult;
}
esp_err_t esp_vfs_fat_sdcard_unmount(const char*, sdmmc_card_t* c) {
  assert(c == &card && ldoCount == 1 && powerLevel == 0);
  calls.push_back("unmount");
  capturedHost.deinit_p(capturedHost.slot);
  return unmountResult;
}
esp_err_t esp_vfs_fat_sdcard_format(const char*, sdmmc_card_t*) {
  assert(false); // the IDF helper has ambiguous ownership on failure
  return ESP_FAIL;
}
BYTE ff_diskio_get_pdrv_card(const sdmmc_card_t*) { return 1; }
FRESULT f_getfree(const char* path, DWORD* free, FATFS** out) {
  assert(std::string(path) == "1:"); // no hardcoded drive 0 assumption
  *free = 1; *out = &volume;
  return FR_OK;
}
FRESULT f_mount(FATFS* fs, const char*, int) {
  if (!fs) return FR_OK;
  assert(fs == &volume);
  return fatRemountResult;
}
FRESULT f_mkfs(const char*, const MKFS_PARM* options, void*, unsigned len) {
  assert(options->fmt == FM_FAT32 && len >= 512);
  ++formats;
  return formatResult;
}
esp_err_t esp_vfs_fat_info(const char* path, uint64_t* total, uint64_t* free) {
  assert(std::string(path) == "/sd");
  *total = 1000; *free = 300;
  return ESP_OK;
}

#if SD_MMC_DETECT_PIN >= 0
// UP-06: the card watch. The policy is the production header; the glue below
// does what VFS::sdWatchTick and its mount job do with it (read the switch
// through the driver, latch a removal, run SD.end() + SD.begin() for a mount),
// over this file's fake switch and mount results. No filesystem lock exists
// here: the three driver calls the tick makes are the lock-free ones.
namespace watch {
sdwatch::State state;
uint32_t now = 0;
bool vfsMounted = false;   // VFS's gSdMounted
bool busy = false;         // a recording, a transcription or a registered owner
int removals = 0, remounts = 0, remountsOk = 0;

// One pass of the main loop at `now`.
void tick() {
  const bool present = gSdMmcCard.cardPresent();
  sdwatch::look(state, present, now);
  if (!sdwatch::sampleDue(state, now)) return;
  const sdwatch::Input in{now, present, vfsMounted, gSdMmcCard.removalLatched()};
  const sdwatch::Output out = sdwatch::sample(state, in);
  if (out.noteRemoved) { gSdMmcCard.noteRemoved(); ++removals; }
  if (out.requestMount) {
    if (!sdwatch::mountStillWanted(state)) return;
    if (busy) { sdwatch::mountSkipped(state, now); return; }
    ++remounts;
    const bool ended = gSdMmcCard.end();          // VFS::remountSd
    vfsMounted = false;
    const bool ok = ended && gSdMmcCard.begin();
    vfsMounted = ok;
    if (ok) ++remountsOk;
    sdwatch::mountResult(state, ok, now);
  }
}
// The loop runs for `ms`, a pass every 10 ms.
void run(uint32_t ms) { for (uint32_t t = 0; t < ms; t += 10) { now += 10; tick(); } }
void userUnmount() {   // sdunmount: VFS::unmountSd
  gSdMmcCard.end();
  vfsMounted = false;
  if (!sdwatch::userUnmounted(state, now)) ++removals;  // the unmount event, unless the pull was reported
}
void userMount() {     // sdmount: VFS::remountSd + the command's note
  gSdMmcCard.end();
  const bool ok = gSdMmcCard.begin();
  vfsMounted = ok;
  sdwatch::userMounted(state, ok, now);
}
}  // namespace watch

// The policy alone: what counts as an empty slot.
static void slotEmptyRule() {
  using sdwatch::State;
  State s{};
  // One open read, even at the sample itself, is not an empty slot.
  sdwatch::look(s, true, 1000);
  (void)sdwatch::sample(s, {1000, true, true, false});
  sdwatch::look(s, false, 1500);
  assert(!sdwatch::slotEmpty(s, 1500));
  sdwatch::Output out = sdwatch::sample(s, {1500, false, true, false});
  assert(!out.noteRemoved && !out.requestMount && s.presentRun == 0);
  // Open at every look for 100 ms: empty, reported once.
  sdwatch::look(s, false, 1550);
  assert(!sdwatch::slotEmpty(s, 1550));        // two looks, but only 50 ms
  sdwatch::look(s, false, 1600);
  assert(sdwatch::slotEmpty(s, 1600));
  sdwatch::look(s, false, 2000);
  out = sdwatch::sample(s, {2000, false, true, false});
  assert(out.noteRemoved && !out.requestMount);
  sdwatch::look(s, false, 2500);
  out = sdwatch::sample(s, {2500, false, true, true});
  assert(!out.noteRemoved);                    // once per mount
  // A present look in between starts the 100 ms over.
  s = State{};
  sdwatch::look(s, false, 100);
  sdwatch::look(s, true, 150);
  sdwatch::look(s, false, 190);
  sdwatch::look(s, false, 240);
  assert(!sdwatch::slotEmpty(s, 240));         // 50 ms since the break
  sdwatch::look(s, false, 290);
  assert(sdwatch::slotEmpty(s, 290));
  // The hold and the give-up end on an empty slot only.
  s = State{};
  s.sampled = true; s.userUnmounted = true; s.gaveUp = true; s.failures = 4;
  sdwatch::look(s, false, 600);
  (void)sdwatch::sample(s, {600, false, false, false});
  assert(s.userUnmounted && s.gaveUp && s.failures == 4);
  sdwatch::look(s, false, 1100);
  (void)sdwatch::sample(s, {1100, false, false, false});
  assert(!s.userUnmounted && !s.gaveUp && s.failures == 0);
  // The driver's latch (a card access that found the switch open) is reported
  // as before, with the switch reading closed.
  s = State{};
  sdwatch::look(s, true, 500);
  out = sdwatch::sample(s, {500, true, true, true});
  assert(out.noteRemoved);
}

static void cardWatch() {
  using namespace watch;
  slotEmptyRule();
  // Boot: no card. Nothing is mounted, nothing is asked for.
  inserted = false; mountResult = ESP_OK;
  assert(gSdMmcCard.end());
  state = sdwatch::State{}; now = 1000; vfsMounted = gSdMmcCard.begin();
  assert(!vfsMounted);
  run(10000);
  assert(remounts == 0 && removals == 0);

  // A card put in after boot and left in: one mount, 1 to 2 s later (three
  // undisturbed samples 500 ms apart; the first may still hold the insertion).
  inserted = true;
  run(900);
  assert(remounts == 0);                       // not yet a second
  run(1200);
  assert(remounts == 1 && remountsOk == 1 && vfsMounted && gSdMmcCard.available());
  run(600000);
  assert(remounts == 1 && removals == 0);      // a healthy mount is left alone

  // Pulled: the removal is latched and reported once, nothing is mounted.
  inserted = false;
  run(5000);
  assert(removals == 1 && remounts == 1 && gSdMmcCard.removalLatched() && !gSdMmcCard.available());
  // Put back and held: one remount; the stale mount is released first.
  inserted = true;
  calls.clear();
  run(2000);
  assert(remounts == 2 && remountsOk == 2 && removals == 1 && gSdMmcCard.available() && !gSdMmcCard.removalLatched());
  assert(calls.size() >= 4 && calls[0] == "unmount");  // SD.end() of the stale mount, then the new one
  run(60000);
  assert(remounts == 2);

  // A bouncing contact: present at every sample, absent in between. No mount.
  inserted = false;
  run(3000);
  assert(removals == 2);
  for (int i = 0; i < 400; ++i) {              // 4 s of chatter, 30 ms open in every 100 ms
    inserted = (i % 10) >= 3;
    now += 10; tick();
  }
  assert(remounts == 2 && vfsMounted);         // still the stale, latched mount: nothing new
  inserted = true;
  run(900);
  assert(remounts == 2);                       // the second counts from the last bounce
  run(1200);
  assert(remounts == 3 && remountsOk == 3);

  // A contact that opens for a moment on a healthy mount (30 ms in every
  // 100 ms, for 4 s) is not a removal: nothing is reported, the mount is not
  // marked stale, nothing is mounted again. Before, one open read between two
  // samples reported the card gone and remounted it.
  for (int i = 0; i < 400; ++i) {
    inserted = (i % 10) >= 3;
    now += 10; tick();
  }
  inserted = true;
  run(5000);
  assert(removals == 2 && remounts == 3 && vfsMounted && !gSdMmcCard.removalLatched() && gSdMmcCard.available());

  // sdunmount holds the watch while the card stays in.
  userUnmount();
  assert(removals == 3);                       // the unmount itself is reported
  // The driver has released the slot, which resets the detect pin; it is
  // armed again, so the switch still shows what is in the slot.
  assert(detectArmed && gSdMmcCard.cardPresent());
  run(600000);
  assert(remounts == 3 && !vfsMounted);
  // The same chatter does not end the hold either.
  for (int i = 0; i < 400; ++i) {
    inserted = (i % 10) >= 3;
    now += 10; tick();
  }
  inserted = true;
  run(10000);
  assert(remounts == 3 && !vfsMounted && sdwatch::phase(state) == sdwatch::Phase::Held);
  // sdmount ends the hold.
  userMount();
  assert(vfsMounted);
  run(10000);
  assert(remounts == 3);
  // sdunmount, card out, card in: a new insertion is mounted (the safe way to swap cards).
  userUnmount();
  assert(removals == 4);
  inserted = false;
  assert(!gSdMmcCard.cardPresent());           // seen although the slot was released by the unmount
  run(2000);
  assert(removals == 4);                       // nothing mounted: no second report
  assert(sdwatch::phase(state) == sdwatch::Phase::Watching);  // the empty slot ended the hold
  inserted = true; run(2100);
  assert(remounts == 4 && remountsOk == 4 && vfsMounted);

  // Pulled first, sdunmount afterwards (to release the stale mount): one report.
  inserted = false; run(1000);
  assert(removals == 5);
  userUnmount();
  assert(removals == 5);
  assert(sdwatch::phase(state) == sdwatch::Phase::Watching);  // an empty slot is not held
  inserted = true; run(2100);
  assert(remounts == 5 && vfsMounted);

  // A card that cannot be mounted: 1 s, then 5 s, 30 s and 5 min later, then no more.
  inserted = false; run(1000);
  mountResult = ESP_FAIL;
  inserted = true;
  const int before = remounts;
  run(2100);  assert(remounts == before + 1 && !vfsMounted);
  run(2800);  assert(remounts == before + 1);  // the first try was at most 2.1 s ago: not 5 s yet
  run(2800);  assert(remounts == before + 2);  // 5 s after it (the next 500 ms sample)
  run(27000); assert(remounts == before + 2);
  run(4000);  assert(remounts == before + 3);  // 30 s after the second
  run(295000); assert(remounts == before + 3);
  run(6000);  assert(remounts == before + 4);  // 5 min after the third

  run(3600000);
  assert(remounts == before + 4 && sdwatch::phase(state) == sdwatch::Phase::GaveUp);
  assert(ldoCount == 0 && powerLevel == 1);    // every failed mount released the card's power
  assert(detectArmed);                         // and left the detect switch readable
  // A contact bounce does not start the four attempts over.
  for (int i = 0; i < 400; ++i) {
    inserted = (i % 10) >= 3;
    now += 10; tick();
  }
  inserted = true;
  run(600000);
  assert(remounts == before + 4 && sdwatch::phase(state) == sdwatch::Phase::GaveUp);
  // Taken out and put back (now readable): mounted again.
  inserted = false; run(1000);
  mountResult = ESP_OK;
  inserted = true; run(2100);
  assert(remounts == before + 5 && vfsMounted);

  // In use: no mount is started and the backoff does not advance.
  inserted = false; run(1000);
  inserted = true; busy = true;
  const int beforeBusy = remounts;
  run(30000);
  assert(remounts == beforeBusy && state.failures == 0 && !state.gaveUp);
  busy = false;
  run(2600);                                   // a skipped mount is looked at again within 2 s
  assert(remounts == beforeBusy + 1 && vfsMounted);

  // A card present at boot that boot could not mount counts as the first
  // failed attempt: no second try at once, then the same backoff.
  inserted = false; run(1000);
  assert(gSdMmcCard.end());
  inserted = true; mountResult = ESP_FAIL;
  state = sdwatch::State{}; vfsMounted = gSdMmcCard.begin();
  assert(!vfsMounted);
  const int beforeBoot = remounts;
  run(4900); assert(remounts == beforeBoot);
  run(700);  assert(remounts == beforeBoot + 1);   // 5 s after boot
  run(31000); assert(remounts == beforeBoot + 2);
  run(301000); assert(remounts == beforeBoot + 3);
  run(3600000); assert(remounts == beforeBoot + 3 && state.gaveUp);
  // sdmount by hand starts over (and here succeeds).
  mountResult = ESP_OK;
  userMount();
  assert(vfsMounted && !state.gaveUp && state.failures == 0);
  assert(gSdMmcCard.end());
  mounts = 0; calls.clear();
}
#endif

int main() {
#if SD_MMC_DETECT_PIN >= 0
  cardWatch();
#endif
  // Empty slot is optional: no FAT/host mount, no retained LDO or card power.
  inserted = false;
#if SD_MMC_DETECT_PIN >= 0
  assert(!gSdMmcCard.begin() && mounts == 0 && ldoCount == 0 && powerLevel == 1);
#else
  // Without a switch we must try the bus; a failed probe releases slot/power.
  assert(!gSdMmcCard.cardPresent());
  mountResult = ESP_FAIL;
  assert(!gSdMmcCard.begin() && mounts == 1 && ldoCount == 0 && powerLevel == 1);
  assert(!capturedFormat && formats == 0 && !gSdMmcCard.cardPresent());
  mountResult = ESP_OK;
  mounts = 0;
#endif
  inserted = true;
  ldoResult = ESP_FAIL;
  assert(!gSdMmcCard.begin() && mounts == 0 && ldoCount == 0 && powerLevel == 1);
  ldoResult = ESP_OK;

  // A corrupt/unformatted card is preserved; repeated failures release power.
  mountResult = ESP_FAIL;
  for (int i = 0; i != 3; ++i) {
    assert(!gSdMmcCard.begin() && !capturedFormat && formats == 0);
    assert(ldoCount == 0 && powerLevel == 1);
  }
  mountResult = ESP_OK;
  assert(gSdMmcCard.begin() && gSdMmcCard.available());
  assert(!capturedFormat && formats == 0);
  assert(gSdMmcCard.totalBytes() == 1000 && gSdMmcCard.usedBytes() == 700);
  const int mountedCount = mounts;
  assert(gSdMmcCard.begin() && mounts == mountedCount); // idempotent

  // An observed removal stays unavailable after reinsertion until remount.
  // (Nothing in the driver mounts; with a detect switch the card watch asks
  // VFS for that remount: see cardWatch() below.)
  inserted = false;
#if SD_MMC_DETECT_PIN >= 0
  assert(!gSdMmcCard.removalLatched());  // nobody has looked yet
  assert(!gSdMmcCard.available());
  assert(gSdMmcCard.removalLatched());
  inserted = true;
  assert(!gSdMmcCard.available());
  assert(gSdMmcCard.removalLatched() && gSdMmcCard.cardPresent());
#else
  // No GPIO can signal removal: retain mount state until explicit unmount.
  assert(gSdMmcCard.available());
  inserted = true;
#endif
  calls.clear();
  assert(gSdMmcCard.end());
  assert((calls == std::vector<std::string>{"unmount", "release-slot", "power-off", "release-ldo"}));
  assert(ldoCount == 0 && gSdMmcCard.begin() && gSdMmcCard.available());

  // Only the explicit format entry permits format-on-mount and erases a valid volume.
  assert(gSdMmcCard.format() && capturedFormat && formats == 1);
  formatResult = ESP_FAIL;
  assert(!gSdMmcCard.format());
  assert(!gSdMmcCard.available() && ldoCount == 0 && powerLevel == 1);
  assert(gSdMmcCard.end() && ldoCount == 0); // idempotent failure cleanup
  formatResult = ESP_OK;
  fatRemountResult = ESP_FAIL;
  assert(!gSdMmcCard.format()); // successful erase but failed FAT remount
  assert(!gSdMmcCard.available() && ldoCount == 0 && powerLevel == 1);
  fatRemountResult = FR_OK;
  assert(gSdMmcCard.begin());
  unmountResult = ESP_FAIL; // SDK has consumed card before unregister error
  assert(!gSdMmcCard.end() && ldoCount == 0 && !gSdMmcCard.available());
  assert(gSdMmcCard.end()); // no second SDK unmount of the consumed pointer
}
