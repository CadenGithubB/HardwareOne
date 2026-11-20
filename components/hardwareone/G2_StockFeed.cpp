// Stock G2 stock widget fed with system metrics. See G2_StockFeed.h.
#include "G2_StockFeed.h"

#if ENABLE_BLUETOOTH && ENABLE_G2_GLASSES

#include <Arduino.h>
#include <atomic>
#include <new>
#include <freertos/FreeRTOS.h>

#include "G2_StockFeedCore.h"
#include "System_Debug.h"
#include "System_MemUtil.h"
#include "System_Metrics.h"
#include "System_Settings.h"
#include "System_Utils.h"

namespace sf = hw1_g2_stock_feed;
namespace feed = hw1_g2_feed;
namespace mc = system_metrics_core;

namespace {

constexpr uint32_t kManualFloorMs = 1000;  // `g2 stats send` and the clear
constexpr uint32_t kWarmHoldMs = 60000;    // preview/send keep the sampler running this long
// A manual send waits this long for a first sample, counted from the owner
// lap that takes the request (the sampler opens in that lap).
constexpr uint32_t kForceWaitMs = 10000;
// Failed writes of one `feed off` clear before it is left to the next
// connect (whose session prelude clears the widget anyway).
constexpr uint8_t kClearMaxTries = 5;
// Owner lap caps (g2StockFeedOwnerWaitMs): a request waiting for its first
// sample or the clear floor, and a batch in flight (its ack timeout).
constexpr uint32_t kRequestLapMs = 500;
constexpr uint32_t kBatchLapMs = 1000;

// The command side's reply text. Its tail (the last sizeof(Slot) bytes) holds
// the slot of the row being formatted, so rows stop at kRowsBytes; the
// preview then builds slot 0 there, encodes it right after the text, moves the
// payload to the end (over the slot) and prints its hex in place. Sized for
// the preview: 9 rows (~80 B each) + a 185-point slot 0 as hex (~1.7 KB).
constexpr size_t kTextBytes = 2688;
constexpr size_t kRowsBytes = kTextBytes - sizeof(sf::Slot);
static_assert(kRowsBytes % alignof(sf::Slot) == 0, "the slot tail must be aligned");
static_assert(kRowsBytes >= 1024, "room for the status header and 9 rows");

bool feedOn() { return gSettings.g2StockFeed; }
uint32_t intervalSec() { return sf::clampIntervalSec(gSettings.g2StockIntervalSec); }
sf::Pad padMode() { return sf::padFromSetting(gSettings.g2StockPad); }

// One PSRAM block, allocated on first use by the feed or by `g2 stats
// status|preview|send`, and kept: the owner's slot and payload, and the
// command task's reply text with its slot tail (about 4.3 KB in all).
struct Scratch {
  sf::Slot ownerSlot;
  uint8_t ownerPb[sf::kFeedPbBytes];
  alignas(sf::Slot) char cmd[kTextBytes];
};
std::atomic<Scratch*> sScratch{nullptr};

Scratch* scratch() {
  Scratch* s = sScratch.load(std::memory_order_acquire);
  if (s) return s;
  void* mem = ps_alloc(sizeof(Scratch), AllocPolicy::RequirePsram, "g2.stockfeed");
  if (!mem) return nullptr;
  Scratch* fresh = new (mem) Scratch();
  Scratch* expected = nullptr;
  if (sScratch.compare_exchange_strong(expected, fresh, std::memory_order_acq_rel)) return fresh;
  ps_free(mem);  // the other task won
  return expected;
}

// The command task's slot, in the reply text's tail (see kRowsBytes).
sf::Slot& cmdSlot(Scratch& sc) { return *new (sc.cmd + kRowsBytes) sf::Slot(); }

// Short replies (`g2 stats interval|metrics`) that need no scratch block.
EXT_RAM_BSS_ATTR static char sReply[192];

// ---- owned by the G2 control task (tick + echoes)
feed::Policy<uint32_t> sPolicy;
uint8_t sMagic = 0;
SystemMetricId sBatchIds[sf::kMaxSlots];
sf::Batch sBatch;
bool sBatchManual = false;
sf::GlassesCount sCount;
bool sPageOpen = false;
uint32_t sPageOpenAtMs = 0;
// The sampler consumer: the owner opens it; the owner or `g2 stats feed off`
// (which must end the run even while the owner is stopped) closes it.
std::atomic<uint8_t> sMetricsHandle{0};
uint32_t sMetricsIntervalMs = 0;  // owner only
bool sFastLapOk = false;  // last lap: right temple open and firmware supported
// The `feed off` clear.
uint8_t sClearMagic = 0;
uint32_t sClearGeneration = 0;
bool sClearAwaiting = false;
uint32_t sClearLastMs = 0;
bool sClearTried = false;
uint8_t sClearFailures = 0;
// A manual send: when this task first saw it.
bool sForceSeen = false;
uint32_t sForceAtMs = 0;
// The version gate's one BASIC request per link.
uint32_t sFwGeneration = 0;
uint32_t sFwSinceMs = 0;
bool sFwAsked = false;

// ---- requests from commands (any task)
std::atomic<bool> sForce{false};
std::atomic<bool> sClearPending{false};
std::atomic<bool> sResync{false};
std::atomic<uint32_t> sWarmUntilMs{0};
std::atomic<bool> sWarm{false};

// ---- read by commands
struct Status {
  feed::Counters feed;
  bool pageOpen;
  bool firmwareOk;
  bool firmwareAsked;
  char firmware[16];
  uint8_t lastCount;
  uint32_t rejected;
  uint32_t clears;
  uint32_t clearAcks;
  uint32_t pageOpens;
  bool lastWasManual;
  bool noPsram;
  sf::Selection selection;
  bool selectionLoaded;
};
Status sStatus{};
portMUX_TYPE sStatusMux = portMUX_INITIALIZER_UNLOCKED;

// The saved metric list, parsed once; commands republish it on change.
sf::Selection selection() {
  sf::Selection s;
  bool loaded;
  portENTER_CRITICAL(&sStatusMux);
  loaded = sStatus.selectionLoaded;
  s = sStatus.selection;
  portEXIT_CRITICAL(&sStatusMux);
  if (loaded) return s;
  s = sf::defaultSelection();
  char err[48];
  if (!sf::parseMetricList(gSettings.g2StockMetrics.c_str(), s, err, sizeof err))
    s = sf::defaultSelection();
  portENTER_CRITICAL(&sStatusMux);
  if (!sStatus.selectionLoaded) {
    sStatus.selection = s;
    sStatus.selectionLoaded = true;
  }
  s = sStatus.selection;
  portEXIT_CRITICAL(&sStatusMux);
  return s;
}

void publishSelection(const sf::Selection& s) {
  portENTER_CRITICAL(&sStatusMux);
  sStatus.selection = s;
  sStatus.selectionLoaded = true;
  portEXIT_CRITICAL(&sStatusMux);
}

bool warmActive(uint32_t now) {
  return sWarm.load(std::memory_order_acquire) &&
         (int32_t)(sWarmUntilMs.load(std::memory_order_acquire) - now) > 0;
}

void warmFor(uint32_t now) {
  sWarmUntilMs.store(now + kWarmHoldMs, std::memory_order_release);
  sWarm.store(true, std::memory_order_release);
}

// Owner side: take the manual-send request (true if there was one).
bool takeForce() {
  sForceSeen = false;
  return sForce.exchange(false, std::memory_order_acq_rel);
}

// A metric as the sampler has it, its chart window filled straight from the
// sampler's history into slot.points (sf::makeSlot moves it in place).
// `stale` allows the last history point for a metric that is unavailable this
// sample (a slot already counted into the batch's stock_total must still be
// sent).
bool readSlot(SystemMetricId id, sf::Slot& slot, bool withHistory, bool stale, sf::Pad pad) {
  SystemMetricReading r{};
  const bool have = systemMetricsRead(id, r) && r.available;
  sf::MetricInput in;
  in.id = id;
  in.available = true;
  uint8_t n = 0;
  if (withHistory || (!have && stale)) n = systemMetricsHistory(id, slot.points, sf::kFeedPoints);
  if (have) {
    in.value = r.value;
    in.delta = r.delta;
    in.rangeLow = r.rangeLow;
    in.rangeHigh = r.rangeHigh;
    in.trend = r.trend;
  } else {
    if (!stale) return false;
    const mc::MetricInfo* m = mc::info(id);
    in.value = n ? slot.points[n - 1] : 0.0f;
    in.rangeLow = m ? m->rangeLow : 0.0f;
    in.rangeHigh = m ? m->rangeHigh : 0.0f;
  }
  in.history = withHistory || !have ? slot.points : nullptr;
  in.historyCount = in.history ? n : 0;
  return sf::makeSlot(in, pad, slot);
}

struct Current {
  SystemMetricId ids[sf::kMaxSlots];
  uint8_t count = 0;
  uint32_t digest = 0;
};

// The batch the tile should show now: the saved metrics that have a value,
// in the saved order, and its send-on-change digest (the shown values and the
// history-point count, so a new 20 s point scrolls the chart).
Current currentBatch(sf::Slot& slot) {
  Current cur;
  const sf::Selection sel = selection();
  uint32_t h = sf::digestStart();
  for (uint8_t i = 0; i < sel.count; i++) {
    if (!readSlot(sel.ids[i], slot, false, false, sf::Pad::Gap)) continue;
    cur.ids[cur.count++] = sel.ids[i];
    h = sf::digestSlot(h, sel.ids[i], slot);
  }
  cur.digest = sf::digestPoints(sf::digestAdd(h, cur.count), systemMetricsPointGeneration());
  return cur;
}

void countFailure() {
  portENTER_CRITICAL(&sStatusMux);
  feed::countFailed(sStatus.feed);
  portEXIT_CRITICAL(&sStatusMux);
}

void publishStatus(bool ready, bool busy, bool fwOk, const char* fw) {
  char copy[sizeof sStatus.firmware];
  snprintf(copy, sizeof copy, "%s", fw ? fw : "");
  portENTER_CRITICAL(&sStatusMux);
  feed::countLink(sStatus.feed, sPolicy, ready, busy);
  sStatus.pageOpen = sPageOpen;
  sStatus.firmwareOk = fwOk;
  sStatus.firmwareAsked = sFwAsked;
  memcpy(sStatus.firmware, copy, sizeof copy);
  portEXIT_CRITICAL(&sStatusMux);
}

void releaseConsumer() {
  const uint8_t h = sMetricsHandle.exchange(0, std::memory_order_acq_rel);
  if (h) systemMetricsClose(h);
}

// Sampler interest (sf::samplerWanted): whenever the feed is on, connected or
// not, so the history survives a glasses disconnect; and for a while after
// `g2 stats preview` / `send`. The page-open cadence while the expanded page
// is open on a connected temple (or a preview/send warms it), the feed
// interval otherwise; the history takes a point every 20 s either way.
void updateConsumer(uint32_t now, bool on, bool linkOpen) {
  const bool warm = warmActive(now) || sForce.load(std::memory_order_acquire);
  if (!warm && sWarm.load(std::memory_order_acquire) &&
      (int32_t)(sWarmUntilMs.load(std::memory_order_acquire) - now) <= 0)
    sWarm.store(false, std::memory_order_release);
  if (!sf::samplerWanted(on, warm)) {
    releaseConsumer();
    sMetricsIntervalMs = 0;
    return;
  }
  const uint32_t ms = sf::samplerIntervalMs(sPageOpen && linkOpen, warm, intervalSec());
  const uint8_t h = sMetricsHandle.load(std::memory_order_acquire);
  if (!h) {
    const uint8_t opened = systemMetricsOpen(ms);
    sMetricsIntervalMs = opened ? ms : 0;
    if (opened) sMetricsHandle.store(opened, std::memory_order_release);  // only the owner opens
  } else if (ms != sMetricsIntervalMs && systemMetricsSetInterval(h, ms)) {
    sMetricsIntervalMs = ms;
  }
}

}  // namespace

// The G2 control owner is stopping (closeg2): no lap runs until the next
// openg2, so nothing would expire the page-open cadence or a preview's
// warm-up. Keep only the feed's own interest: the feed interval if it is on
// (the history keeps its 20 s clock), nothing if it is off.
void g2StockFeedOwnerStopped() {
  sPageOpen = false;
  sFastLapOk = false;
  sWarm.store(false, std::memory_order_release);
  if (!feedOn()) {
    releaseConsumer();
    sMetricsIntervalMs = 0;
    return;
  }
  const uint32_t ms = sf::samplerIntervalMs(false, false, intervalSec());
  const uint8_t h = sMetricsHandle.load(std::memory_order_acquire);
  if (h && ms != sMetricsIntervalMs && systemMetricsSetInterval(h, ms)) sMetricsIntervalMs = ms;
}

namespace {

// The version gate needs the right temple's firmware string. If no settings
// push brought it a few seconds after the link came up, ask once per link.
void askFirmware(uint32_t generation, uint32_t now, const char* fw, bool wanted) {
  if (generation != sFwGeneration) {
    sFwGeneration = generation;
    sFwSinceMs = now;
    sFwAsked = false;
  }
  if (fw[0] || sFwAsked || !wanted) return;
  if ((uint32_t)(now - sFwSinceMs) < sf::kFirmwareQueryDelayMs) return;
  sFwAsked = true;
  const bool ok = g2StockFeedRequestFirmware(generation);
  DEBUG_G2F("[G2-STOCK] right temple firmware unknown: settings request %s",
            ok ? "sent" : "NOT sent");
}

void abortBatch() {
  sf::batchStop(sBatch);
  sPolicy.awaitingAck = false;
}

bool writeSlot(Scratch& sc, uint8_t index, uint32_t generation) {
  const sf::Pad pad = padMode();
  if (!readSlot(sBatchIds[index], sc.ownerSlot, true, true, pad)) return false;
  const uint8_t magic = sf::magicAt(sBatch.first, index);
  const size_t n = sf::encodeSlot(sc.ownerSlot, sBatch.count, index, magic, sc.ownerPb,
                                  sizeof sc.ownerPb);
  const bool gap = pad == sf::Pad::Gap;
  const bool ok = n && sf::validateMessagePb(sc.ownerPb, n, nullptr, gap) &&
                  g2StockFeedWireSend(generation, sc.ownerPb, n, gap);
  DEBUG_G2F("[G2-STOCK] slot %u/%u %s %.2f %u/%u pts magic=%u len=%u %s", (unsigned)index,
            (unsigned)sBatch.count, sc.ownerSlot.code, (double)sc.ownerSlot.value,
            (unsigned)sc.ownerSlot.realPoints, (unsigned)sc.ownerSlot.pointCount,
            (unsigned)magic, (unsigned)n, ok ? "sent" : "NOT sent");
  return ok;
}

// The batch's next write, if its turn has come (sf::batchStep). The first
// write registers the batch with the send policy as one unit; a failed write
// gives the batch up and lets the same data go again after the interval.
void runBatch(Scratch& sc, uint32_t generation, uint32_t now) {
  const sf::Step step = sf::batchStep(sBatch, generation, sPolicy.awaitingAck);
  if (step == sf::Step::Wait) return;
  if (step == sf::Step::Stop) {
    sf::batchStop(sBatch);  // complete, timed out (the policy retries) or old link
    return;
  }
  bool ok = false;
  if (step == sf::Step::Clear) {
    const size_t n = sf::encodeClear(sBatch.clearMagic, sc.ownerPb, sizeof sc.ownerPb);
    ok = n && g2StockFeedWireSend(generation, sc.ownerPb, n, false);
    DEBUG_G2F("[G2-STOCK] clear before the batch (last one unfinished) magic=%u %s",
              (unsigned)sBatch.clearMagic, ok ? "sent" : "NOT sent");
  } else {
    ok = writeSlot(sc, sBatch.next, generation);
  }
  if (!ok) {
    feed::unitFailed(sPolicy, now);
    sf::batchStop(sBatch);
    countFailure();
    return;
  }
  const bool firstWrite = !sBatch.wrote;
  if (step == sf::Step::Slot) sf::countSlotWritten(sCount, generation);
  sf::batchWrote(sBatch, step);
  if (firstWrite) feed::sent(sPolicy, now, sBatch.first, sBatch.count);
  else feed::extend(sPolicy, now);
  portENTER_CRITICAL(&sStatusMux);
  if (firstWrite) {
    feed::countSent(sStatus.feed, now, sBatch.first);
    sStatus.lastCount = sBatch.count;
    sStatus.lastWasManual = sBatchManual;
  }
  if (step == sf::Step::Clear) sStatus.clears++;
  portEXIT_CRITICAL(&sStatusMux);
}

void startBatch(Scratch& sc, const Current& cur, uint32_t generation, uint32_t now, bool manual) {
  // The glasses' count is part-way after an unfinished batch on this link:
  // reset it with a clear first (sf::Batch).
  const bool clearFirst = sf::countPartway(sCount, generation);
  uint8_t clearMagic = 0;
  if (clearFirst) {
    clearMagic = sf::unitMagic(sMagic, 1);
    sMagic = clearMagic;
  }
  const uint8_t first = sf::unitMagic(sMagic, cur.count);
  sMagic = sf::magicAt(first, (uint8_t)(cur.count - 1));
  for (uint8_t i = 0; i < cur.count; i++) sBatchIds[i] = cur.ids[i];
  sf::batchBegin(sBatch, cur.count, first, generation, clearFirst, clearMagic);
  sBatchManual = manual;
  sPolicy.pending = cur.digest;
  runBatch(sc, generation, now);
}

// `g2 stats feed off`: one stock_total = 0. True if it was written.
bool sendClear(Scratch& sc, uint32_t generation, uint32_t now) {
  sClearMagic = sf::unitMagic(sMagic, 1);
  sMagic = sClearMagic;
  const size_t n = sf::encodeClear(sClearMagic, sc.ownerPb, sizeof sc.ownerPb);
  const bool ok = n && g2StockFeedWireSend(generation, sc.ownerPb, n, false);
  sClearTried = true;
  sClearLastMs = now;
  sClearAwaiting = ok;
  sClearGeneration = generation;
  DEBUG_G2F("[G2-STOCK] clear magic=%u %s", (unsigned)sClearMagic, ok ? "sent" : "NOT sent");
  portENTER_CRITICAL(&sStatusMux);
  if (ok) sStatus.clears++;
  else feed::countFailed(sStatus.feed);
  portEXIT_CRITICAL(&sStatusMux);
  return ok;
}

constexpr char kAvailableLabels[] = "CPU CPU0 CPU1 HEAP HBLK PSRM SD LFS TEMP LINK MESH RING";
constexpr char kNoPsram[] = "Error: No PSRAM for the stock widget feed (it needs about 4.5 KB)";

}  // namespace

void g2StockFeedTick() {
  const uint32_t now = millis();
  uint32_t generation = 0;
  bool busy = false;
  char fw[16] = {};
  const bool ready = g2StockFeedRightLink(&generation, &busy, fw, sizeof fw);
  const bool fwOk = sf::firmwareSupported(fw);
  const bool on = feedOn();
  const feed::Gate g = feed::gate(ready, generation, busy);
  sFastLapOk = g == feed::Gate::Open && fwOk;

  if (sForce.load(std::memory_order_acquire) && !sForceSeen) {
    sForceSeen = true;
    sForceAtMs = now;
  }
  if (sPageOpen && !sf::pageOpenCurrent(true, sPageOpenAtMs, now)) {
    // No type-3 push for a while: the lens timed out or the wearer looked
    // down without closing the page. Back to the tile interval.
    sPageOpen = false;
    DEBUG_G2F("[G2-STOCK] stock page: no page state for %u s, treated as closed",
              (unsigned)(sf::kPageOpenHoldMs / 1000));
  }
  if (sResync.exchange(false, std::memory_order_acq_rel)) {
    // Feed switched on, the metric list or the pad changed: a full batch, not
    // a diff.
    abortBatch();
    sPolicy.sentThisLink = false;
    sPolicy.haveAttempted = false;
  }
  // Not gated on the link or the firmware: the sampler (and the hour of
  // history the next connect sends) keeps going while the glasses are away.
  updateConsumer(now, on, g == feed::Gate::Open);

  switch (g) {
    case feed::Gate::Down:
      // A one-shot request never outlives the link it was made for.
      if (takeForce()) countFailure();
      feed::linkDown(sPolicy);
      sf::batchStop(sBatch);
      sPageOpen = false;
      sClearAwaiting = false;
      sClearTried = false;
      sClearFailures = 0;
      publishStatus(false, busy, fwOk, fw);
      return;
    case feed::Gate::Paused:
      // Exclusive glasses sessions pause the feed without resetting the link
      // state, so nothing bursts out when they end.
      publishStatus(true, true, fwOk, fw);
      return;
    case feed::Gate::Open:
      break;
  }
  if (!fwOk) {
    // Version gate: only firmware with a captured fixture gets stock data.
    askFirmware(generation, now, fw, on || sForce.load(std::memory_order_acquire));
    if (takeForce()) countFailure();
    publishStatus(true, false, false, fw);
    return;
  }
  if (!on && !sForce.load(std::memory_order_acquire) &&
      !sClearPending.load(std::memory_order_acquire) && !sBatch.active) {
    // Nothing to do: keep the link bookkeeping, take no PSRAM, read nothing.
    (void)feed::tick(sPolicy, sf::timing(sPageOpen, intervalSec()), false, false, true,
                     generation, now, sPolicy.pending);
    publishStatus(true, false, true, fw);
    return;
  }
  Scratch* sc = scratch();
  if (!sc) {
    if (takeForce()) countFailure();
    portENTER_CRITICAL(&sStatusMux);
    sStatus.noPsram = true;
    portEXIT_CRITICAL(&sStatusMux);
    publishStatus(true, false, true, fw);
    return;
  }

  // Feed switched off: one stock_total = 0 clears the widget. A failed write
  // is tried again (1 s apart) a few times.
  if (sClearPending.load(std::memory_order_acquire)) {
    if (on) {
      sClearPending.store(false, std::memory_order_release);
      sClearFailures = 0;
    } else if (!feed::tooSoon(sClearTried, sClearLastMs, now, kManualFloorMs) &&
               !feed::tooSoon(sPolicy, now, kManualFloorMs)) {
      abortBatch();
      if (sendClear(*sc, generation, now) || ++sClearFailures >= kClearMaxTries) {
        sClearPending.store(false, std::memory_order_release);
        sClearFailures = 0;
      }
    }
  }

  runBatch(*sc, generation, now);

  const Current cur = currentBatch(sc->ownerSlot);
  // Only a sample of the run this feed holds open (never one left over).
  const bool hasData = cur.count > 0 && sMetricsHandle.load(std::memory_order_acquire) != 0;
  const feed::Action action = feed::tick(sPolicy, sf::timing(sPageOpen, intervalSec()), on,
                                         hasData, true, generation, now, cur.digest);
  if (sForce.load(std::memory_order_acquire)) {
    if (!hasData) {
      // Wait for the sampler's first sample, but not forever.
      if ((uint32_t)(now - sForceAtMs) > kForceWaitMs) {
        takeForce();
        countFailure();
      }
    } else if (feed::tooSoon(sPolicy, now, kManualFloorMs)) {
      takeForce();
      countFailure();  // manual sends are bounded too
    } else {
      takeForce();
      startBatch(*sc, cur, generation, now, true);
    }
  } else if (action == feed::Action::Send) {
    startBatch(*sc, cur, generation, now, false);
  }
  publishStatus(true, false, true, fw);
}

uint32_t g2StockFeedOwnerWaitMs() {
  if (!sFastLapOk) return UINT32_MAX;
  if (sForce.load(std::memory_order_acquire) || sClearPending.load(std::memory_order_acquire))
    return kRequestLapMs;
  return sBatch.active ? kBatchLapMs : UINT32_MAX;
}

void g2StockFeedOnRx(char side, uint32_t generation, const uint8_t* pb, size_t len) {
  uint32_t type = 0;
  if (sf::parsePageState(pb, len, type)) {
    // The master temple reports the expanded stock page opening (type 3,
    // again after each scroll) and leaving it by a tap (any other type).
    const bool open = type == sf::kPageTypeStockExpanded;
    if (open && !sPageOpen) {
      portENTER_CRITICAL(&sStatusMux);
      sStatus.pageOpens++;
      portEXIT_CRITICAL(&sStatusMux);
    }
    if (open != sPageOpen)
      DEBUG_G2F("[G2-STOCK] stock page %s (%c)", open ? "open" : "closed", side);
    sPageOpen = open;
    if (open) sPageOpenAtMs = millis();
    return;
  }
  if (side != 'R') return;
  uint8_t magic = 0;
  bool accepted = false;
  if (!sf::parseAck(pb, len, magic, accepted)) return;
  if (sClearAwaiting && magic == sClearMagic && generation == sClearGeneration) {
    sClearAwaiting = false;
    if (accepted) sf::countReset(sCount);
    portENTER_CRITICAL(&sStatusMux);
    if (accepted) sStatus.clearAcks++;
    else sStatus.rejected++;
    portEXIT_CRITICAL(&sStatusMux);
    return;
  }
  if (sBatch.active && sBatch.clearAwaited && magic == sBatch.clearMagic &&
      generation == sBatch.generation) {
    // The clear ahead of a batch: slot 0 follows in this lap's tick. A
    // refused clear holds the batch until the ack timeout retries it.
    if (accepted) {
      sf::batchClearAcked(sBatch, generation, magic);
      sf::countReset(sCount);
    }
    portENTER_CRITICAL(&sStatusMux);
    if (accepted) sStatus.clearAcks++;
    else sStatus.rejected++;
    portEXIT_CRITICAL(&sStatusMux);
    return;
  }
  if (generation != sPolicy.generation || feed::unitIndex(sPolicy, magic) < 0 ||
      !sPolicy.awaitingAck)
    return;  // not ours (the layout message's echo, an old unit)
  if (!accepted) {
    // The glasses refused the message: no ack; the timeout retries once.
    DEBUG_G2F("[G2-STOCK] magic=%u refused by the glasses", (unsigned)magic);
    portENTER_CRITICAL(&sStatusMux);
    sStatus.rejected++;
    portEXIT_CRITICAL(&sStatusMux);
    return;
  }
  const int index = feed::unitIndex(sPolicy, magic);
  const feed::AckResult r = feed::onAck(sPolicy, generation, sf::kCmdAck, sf::kCmdAck, magic);
  if (r == feed::AckResult::Ignored) return;
  sf::batchSlotAcked(sBatch, index);
  if (r == feed::AckResult::Complete) {
    // Every slot counted: the glasses redrew and their count is back at 0.
    sf::batchStop(sBatch);
    sf::countReset(sCount);
    portENTER_CRITICAL(&sStatusMux);
    feed::countAck(sStatus.feed, millis());
    portEXIT_CRITICAL(&sStatusMux);
  }
}

// ------------------------------------------------------------------ commands

namespace {

Status statusCopy() {
  Status s;
  portENTER_CRITICAL(&sStatusMux);
  s = sStatus;
  portEXIT_CRITICAL(&sStatusMux);
  return s;
}

const char* trendWord(uint8_t trend) { return trend == 1 ? "up" : trend == 2 ? "down" : "flat"; }

// Length snprintf actually wrote into `room` bytes.
size_t written(int n, size_t room) {
  if (n < 0 || room == 0) return 0;
  return (size_t)n < room ? (size_t)n : room - 1;
}

// One line per saved metric at out[0..cap): what the tile shows now, or why
// it is left out. Returns the length written. Must end at or before the slot
// tail (kRowsBytes), which readSlot fills for each metric.
size_t formatRows(Scratch& sc, char* out, size_t cap, const sf::Selection& sel) {
  if (!cap) return 0;
  out[0] = '\0';
  size_t off = 0;
  sf::Slot& slot = cmdSlot(sc);
  const sf::Pad pad = padMode();
  for (uint8_t i = 0; i < sel.count && off + 1 < cap; i++) {
    const mc::MetricInfo* m = mc::info(sel.ids[i]);
    if (!m) continue;
    int n;
    if (readSlot(sel.ids[i], slot, true, false, pad)) {
      const sf::Slot& s = slot;
      n = snprintf(out + off, cap - off, "  %-4s %-20s %9.2f %-5s %+8.2f %-4s chart %.0f-%.0f, %u pts\n",
                   s.code, s.name, (double)s.value, m->unit, (double)s.change,
                   trendWord(s.trend), (double)s.low, (double)s.high, (unsigned)s.realPoints);
    } else {
      n = snprintf(out + off, cap - off, "  %-4s %-20s (no value on this device or yet; not shown)\n",
                   m->label, m->longName);
    }
    off += written(n, cap - off);
  }
  return off;
}

const char* setFeed(bool on) {
  setSetting(gSettings.g2StockFeed, on);
  if (on) {
    sClearPending.store(false, std::memory_order_release);
    sResync.store(true, std::memory_order_release);
    g2StockFeedWake();
    return "Glasses stock widget feed ON (saved): system metrics replace the stocks on the "
           "right temple after connect, then on change (g2 stats interval; every 10 s while the "
           "stock page is open). The chart keeps the last hour, also while the glasses are "
           "away. The stock tile must be in the glasses' dashboard layout.";
  }
  sClearPending.store(true, std::memory_order_release);
  // End the sampler run now (it clears the history), also while the G2 owner
  // is stopped; a preview's warm-up no longer holds it.
  sWarm.store(false, std::memory_order_release);
  releaseConsumer();
  g2StockFeedWake();
  return "Glasses stock widget feed off (saved). The chart history is dropped; the widget is "
         "cleared (stock_total 0) on the right temple now, or when it next connects";
}

}  // namespace

const char* cmd_g2statsstatus(const String& /*args*/) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  Scratch* sc = scratch();
  if (!sc) return kNoPsram;
  const Status s = statusCopy();
  const sf::Selection sel = selection();
  char list[64];
  sf::formatMetricList(sel, list, sizeof list);
  const uint32_t now = millis();
  const feed::Counters& f = s.feed;
  char sentAgo[24], ackAgo[24];
  if (f.sends) snprintf(sentAgo, sizeof sentAgo, "%u s ago", (unsigned)((now - f.lastSendMs) / 1000));
  else snprintf(sentAgo, sizeof sentAgo, "never");
  if (f.acks) snprintf(ackAgo, sizeof ackAgo, "%u s ago", (unsigned)((now - f.lastAckMs) / 1000));
  else snprintf(ackAgo, sizeof ackAgo, "never");
  const char* fwNote = s.firmware[0] ? (s.firmwareOk ? " (has a fixture)" : " (no fixture: nothing is sent)")
                       : s.firmwareAsked ? " (asked the glasses; nothing is sent until it is known)"
                                         : " (nothing is sent until it is known)";
  char* buf = sc->cmd;
  size_t off = written(snprintf(
      buf, kRowsBytes,
      "Glasses stock widget feed: %s (saved), every %u s on change, 10 s while the stock page is open\n"
      "Chart: last hour, a point every 20 s, %u points, left padding %s (saved)\n"
      "Right temple: %s%s; firmware %s%s\n"
      "Stock page: %s (opened %u times)\n"
      "Metrics (saved): %s\n",
      feedOn() ? "ON" : "off", (unsigned)intervalSec(), (unsigned)sf::kFeedPoints,
      sf::padName(padMode()),
      f.linkReady ? "ready" : "not ready",
      f.busy ? " (paused: another glasses session is active)" : "",
      s.firmware[0] ? s.firmware : "unknown", fwNote,
      s.pageOpen ? "open" : "closed", (unsigned)s.pageOpens, list), kRowsBytes);
  if (systemMetricsGeneration() == 0) {
    off += written(snprintf(buf + off, kRowsBytes - off,
                            "  (no sample yet: the sampler runs while the feed is on, from the "
                            "first glasses connect after boot, or after g2 stats preview)\n"),
                   kRowsBytes - off);
  } else {
    off += formatRows(*sc, buf + off, kRowsBytes - off, sel);
  }
  // The rows are done with the history tail: the counters may use all of it.
  snprintf(buf + off, kTextBytes - off,
           "Batches sent %u (last %u slots%s), fully acked %u, failed %u, refused %u; last send %s, "
           "last ack %s%s\nClears sent %u, acked %u%s%s",
           (unsigned)f.sends, (unsigned)s.lastCount, s.lastWasManual ? ", manual" : "",
           (unsigned)f.acks, (unsigned)f.sendFailures, (unsigned)s.rejected, sentAgo, ackAgo,
           f.awaitingAck ? "; waiting for the glasses' acks" : "", (unsigned)s.clears,
           (unsigned)s.clearAcks,
           sClearPending.load() ? "; a clear is pending" : "",
           s.noPsram ? "\nError: no PSRAM block for the feed" : "");
  return buf;
}

const char* cmd_g2statspreview(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String a = args;
  a.trim();
  if (a.length()) return "Error: invalid arguments — Usage: g2 stats preview";
  Scratch* sc = scratch();
  if (!sc) return kNoPsram;
  warmFor(millis());  // keep the sampler running for a minute
  g2StockFeedWake();
  if (systemMetricsGeneration() == 0)
    return "No sample yet: the metrics sampler is starting. Run g2 stats preview again in a few "
           "seconds.";
  const sf::Selection sel = selection();
  char* buf = sc->cmd;
  size_t off = written(snprintf(buf, kRowsBytes,
                                "Stock widget rows (code, name, value, change, trend, chart):\n"),
                       kRowsBytes);
  off += formatRows(*sc, buf + off, kRowsBytes - off, sel);
  // Slot 0 of the batch, as it would go on the wire.
  uint8_t total = 0, firstShown = 0xFF;
  for (uint8_t i = 0; i < sel.count; i++) {
    SystemMetricReading r{};
    if (systemMetricsRead(sel.ids[i], r) && r.available) {
      if (firstShown == 0xFF) firstShown = i;
      total++;
    }
  }
  const sf::Pad pad = padMode();
  sf::Slot& slot = cmdSlot(*sc);
  if (firstShown == 0xFF || !readSlot(sel.ids[firstShown], slot, true, false, pad)) {
    snprintf(buf + off, kTextBytes - off, "Nothing to send: no saved metric has a value yet.");
    return buf;
  }
  // Encode right after the text, short of the slot in the tail; then move the
  // payload to the end of the buffer (over the slot, no longer needed) and
  // print its hex in place: two characters out per byte consumed, so it fits
  // whenever the finished text does.
  uint8_t* raw = reinterpret_cast<uint8_t*>(buf);
  const size_t room = off + 1 < kRowsBytes ? kRowsBytes - off - 1 : 0;
  const size_t n = room ? sf::encodeSlot(slot, total, 0, 0x01, raw + off + 1,
                                         room < sf::kFeedPbBytes ? room : sf::kFeedPbBytes)
                        : 0;
  if (!n) {
    snprintf(buf + off, kTextBytes - off, "Slot 0 does not encode (whitelist FAIL).");
    return buf;
  }
  const unsigned realPoints = slot.realPoints, pointCount = slot.pointCount;
  uint8_t* pb = raw + kTextBytes - n;
  memmove(pb, raw + off + 1, n);
  const bool valid = sf::validateMessagePb(pb, n, nullptr, pad == sf::Pad::Gap);
  char line[160];
  const size_t lineLen = written(
      snprintf(line, sizeof line,
               "%u slots; slot 0: sid 0x01, %u B, %u fragment(s), chart %u points (%u real, "
               "%u %s padding), whitelist %s (not sent):\n",
               (unsigned)total, (unsigned)n, (unsigned)(n / 232 + 1), pointCount, realPoints,
               pointCount > realPoints && realPoints ? pointCount - realPoints : 0u,
               sf::padName(pad), valid ? "pass" : "FAIL"),
      sizeof line);
  if (off + lineLen + 2 * n + 1 > kTextBytes) {
    snprintf(buf + off, kTextBytes - off, "%s(the bytes do not fit this reply)", line);
    return buf;
  }
  memcpy(buf + off, line, lineLen);
  off += lineLen;
  const char* digits = "0123456789abcdef";
  for (size_t i = 0; i < n; i++) {
    const uint8_t b = pb[i];
    buf[off++] = digits[b >> 4];
    buf[off++] = digits[b & 0x0F];
  }
  buf[off] = '\0';
  return buf;
}

const char* cmd_g2statssend(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String a = args;
  a.trim();
  if (a.length()) return "Error: invalid arguments — Usage: g2 stats send";
  // g2StockFeedTick drops a request it cannot send and only counts it, so the
  // cases that are known now are refused here with their reason.
  char fw[16] = {};
  if (!g2StockFeedRightLink(nullptr, nullptr, fw, sizeof fw))
    return "Error: Right temple is not ready, nothing sent. Run openg2, then retry.";
  Scratch* sc = scratch();
  if (!sc) return kNoPsram;
  if (!sf::firmwareSupported(fw)) {
    snprintf(sc->cmd, kTextBytes,
             "Error: Glasses firmware %s has no stock fixture (only 2.3.2.14 is fed), nothing sent.",
             fw[0] ? fw : "unknown");
    return sc->cmd;
  }
  const Status s = statusCopy();
  const uint32_t now = millis();
  if (feed::tooSoon(s.feed.attemptedThisLink, s.feed.lastAttemptMs, now, kManualFloorMs))
    return "Error: A batch was sent or tried less than a second ago. Wait a second and retry.";
  warmFor(now);
  sForce.store(true, std::memory_order_release);
  g2StockFeedWake();
  return systemMetricsGeneration() == 0
             ? "Queued one batch of the saved metrics to the right temple; it goes out after the "
               "sampler's first sample (a few seconds). g2 stats status shows the acks."
             : "Queued one batch of the saved metrics to the right temple. g2 stats status shows "
               "the acks.";
}

const char* cmd_g2statsfeed(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String a = args;
  a.trim();
  a.toLowerCase();
  if (!a.length())
    return feedOn() ? "Glasses stock widget feed: on" : "Glasses stock widget feed: off";
  if (argIsOn(a)) return setFeed(true);
  if (argIsOff(a)) return setFeed(false);
  return "Error: invalid arguments — Usage: g2 stats feed [on|off]";
}

const char* cmd_g2statsmetrics(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String a = args;
  a.trim();
  if (!a.length()) {
    char list[64];
    sf::formatMetricList(selection(), list, sizeof list);
    snprintf(sReply, sizeof sReply, "Stock widget metrics (saved, in order): %s\nAvailable: %s (up to 9)",
             list, kAvailableLabels);
    return sReply;
  }
  sf::Selection sel;
  char err[48];
  if (!sf::parseMetricList(a.c_str(), sel, err, sizeof err)) {
    snprintf(sReply, sizeof sReply,
             "Error: invalid arguments — %s. Usage: g2 stats metrics <labels|default>; labels: %s",
             err, kAvailableLabels);
    return sReply;
  }
  char list[64];
  sf::formatMetricList(sel, list, sizeof list);
  // Publish before saving: once a selection is published the G2 task never
  // reads the settings string, so it cannot read it while setSetting writes.
  publishSelection(sel);
  setSetting(gSettings.g2StockMetrics, list);
  sResync.store(true, std::memory_order_release);
  if (feedOn()) g2StockFeedWake();
  snprintf(sReply, sizeof sReply, "Stock widget metrics: %s (saved)%s", list,
           feedOn() ? "; a full batch follows" : "");
  return sReply;
}

const char* cmd_g2statsinterval(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String a = args;
  a.trim();
  if (!a.length()) {
    snprintf(sReply, sizeof sReply,
             "Stock widget feed interval: %u s (10 s while the stock page is open)",
             (unsigned)intervalSec());
    return sReply;
  }
  uint32_t v = 0;
  if (!sf::parseIntervalSec(a.c_str(), v))
    return "Error: invalid arguments — Usage: g2 stats interval [seconds 30-3600]  (default 60)";
  setSetting(gSettings.g2StockIntervalSec, (int)v);
  snprintf(sReply, sizeof sReply, "Stock widget feed interval: %u s (saved)", (unsigned)v);
  return sReply;
}

const char* cmd_g2statspad(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String a = args;
  a.trim();
  if (!a.length()) {
    snprintf(sReply, sizeof sReply,
             "Stock widget chart padding: %s (saved). gap = empty left part, flat = the oldest "
             "point repeated, until an hour of history exists",
             sf::padName(padMode()));
    return sReply;
  }
  sf::Pad pad = sf::Pad::Gap;
  if (!sf::parsePad(a.c_str(), pad))
    return "Error: invalid arguments — Usage: g2 stats pad [gap|flat]  (default gap)";
  setSetting(gSettings.g2StockPad, (int)pad);
  sResync.store(true, std::memory_order_release);
  if (feedOn()) g2StockFeedWake();
  snprintf(sReply, sizeof sReply, "Stock widget chart padding: %s (saved)%s", sf::padName(pad),
           feedOn() ? "; a full batch follows" : "");
  return sReply;
}

#endif  // ENABLE_BLUETOOTH && ENABLE_G2_GLASSES
