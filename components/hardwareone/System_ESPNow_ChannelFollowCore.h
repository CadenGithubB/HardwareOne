#ifndef SYSTEM_ESPNOW_CHANNEL_FOLLOW_CORE_H
#define SYSTEM_ESPNOW_CHANNEL_FOLLOW_CORE_H

// ============================================================================
// Mesh workers follow their master's radio channel (owner design 2026-11-19;
// spec: hw1-work/design/espnow-channel-follow-2026-11-19/SPEC.md).
//
// Pure header (no Arduino, no ESP-IDF, no heap), like System_ESPNow_ChannelCore.h:
// the host test compiles it directly and System_ESPNow.cpp feeds it the radio,
// Wi-Fi and mesh state.
//
//  - Every heartbeat states its sender's channel in byte 3 (chanInfo). Older
//    firmware sends 0 there ("not stated") and never reads it.
//  - A worker in `espnowchannel auto` sits on the last channel its master
//    announced (remembered across reboots). A manual pin 1..13 wins while not
//    joined. While joined, the AP decides; joined elsewhere than the master
//    pauses the mesh until the Wi-Fi link ends (Wi-Fi wins).
//  - Master silent for 30 s, not joined, auto: sweep the saved channel, then
//    1..max, one probe per dwell; lock on the first trusted answer. Failed
//    sweeps repeat after 30 s, 60 s, 120 s, then every 300 s; success resets.
//  - Only a sealed frame may move the radio to another channel than the one it
//    was heard on; a group-signed frame can only confirm the channel it
//    arrived on, and a group-signed frame naming another channel is neither
//    liveness nor a move (adjacent-channel leak or replay). The probe's ACK
//    (group-tagged since 1.0, matched on its ackFor) proves only mesh
//    membership: it parks the sweep on its dwell for 12 s, then the sweep
//    goes on.
//  - An older master (channel not stated) heard during a hunt is a candidate:
//    the sweep dwells two more channels and locks on the strongest signal, so
//    an adjacent-channel leak does not win.
//  - The Wi-Fi rehunt hold needs the primary heard within 30 s and a sealed
//    frame from it within 45 s (a replayed group-signed heartbeat cannot hold
//    Wi-Fi off); while it is
//    wanted, a probe 20 s after the last sealed frame asks the master for one,
//    repeated every 10 s, so one lost probe or answer never drops the hold.
//
// One Follow object (48 bytes, all-zero default state, so it can live in
// EXT_RAM_BSS_ATTR) runs on espnow_task only. Time math is uint32_t(now - then)
// (millis wraparound safe); stamps are kept at most 2^30 ms old by tick(), so a
// silence longer than 24.8 days never reads as "just heard".
// ============================================================================

#include <stdint.h>

#include "System_ESPNow_ChannelCore.h"  // espnowch::validChannel

namespace espnowfollow {

using espnowch::validChannel;

// ---- Heartbeat byte 3 (V4PayloadHeartbeat::chanInfo) -----------------------
inline constexpr uint8_t kChanMask = 0x0F;   // sender's radio channel 1..13; 0 = not stated (older firmware)
inline constexpr uint8_t kProbeBit = 0x10;   // a hunting worker asks its master to answer now
inline constexpr uint8_t kAnswerBit = 0x20;  // this heartbeat answers a probe
// 0xC0 reserved: sent 0, ignored.

inline uint8_t packChanInfo(uint8_t ch, bool probe, bool answer) {
  return uint8_t((validChannel(ch) ? ch : 0) | (probe ? kProbeBit : 0) | (answer ? kAnswerBit : 0));
}
inline uint8_t chanOf(uint8_t info) {  // 14, 15 and 0 read as "not stated"
  const uint8_t c = uint8_t(info & kChanMask);
  return validChannel(c) ? c : 0;
}
inline bool isProbe(uint8_t info) { return (info & kProbeBit) != 0; }
inline bool isAnswer(uint8_t info) { return (info & kAnswerBit) != 0; }

// ---- Timing ---------------------------------------------------------------
inline constexpr uint32_t kSilenceMs = 30000;  // heard nothing for this long -> hunt
inline constexpr uint32_t kGapMs[4] = {30000, 60000, 120000, 300000};  // after 1, 2, 3, 4+ failed sweeps
inline constexpr uint32_t kConfirmMs = 12000;       // after an ACK: wait for a trusted frame (2+ heartbeat periods)
inline constexpr uint32_t kRecheckGapMs = 10000;    // at most one mismatch / keep-alive probe per 10 s
inline constexpr uint32_t kSealedRefreshMs = 20000; // Wi-Fi hold wanted, no sealed frame for this long: probe
inline constexpr uint32_t kWifiHoldMs = 45000;      // Wi-Fi hold: a sealed primary frame this recent (two probe retries)
inline constexpr uint32_t kBusyHoldMaxMs = 60000;   // busy holds the hunt at most this long per busy spell (Q5)
inline constexpr uint32_t kApplyTimeoutMs = 1000;   // a dwell whose switch never reports is skipped
inline constexpr uint16_t kDwellMs = 150;           // native radio
inline constexpr uint16_t kDwellHostedMs = 200;     // P4: one RPC switch + one RPC send inside the dwell
inline constexpr uint8_t kCandidateDwells = 2;      // older master: dwells tried after a candidate
inline constexpr uint32_t kMaxAgeMs = 1u << 30;     // stamps older than this are slid forward (about 12.4 days)

// ---- Inputs and outputs ---------------------------------------------------
enum class Role : uint8_t {
  None,    // mesh off, bond mode, keyless mesh, no master resolved, or a backup that is not promoted
  Worker,  // MESH_ROLE_WORKER with a followed master resolved
  Master,  // MESH_ROLE_MASTER, or a promoted backup: never follows, never hunts
};

enum class Phase : uint8_t {
  Off,         // Role None/Master: espnowch::expected decides (today's behaviour)
  Pinned,      // espnowchannel 1..13: the pin decides; announcements are only remembered
  Joined,      // joined to Wi-Fi on the master's channel (or the master's channel is unknown)
  WifiPaused,  // joined on another channel than the master: mesh paused until the link ends
  Following,   // on the master's channel
  Waiting,     // master silent: next sweep after a gap, or held
  Sweeping,    // hunting: dwell on sweepChannel()
  Confirming,  // an ACK came back on this channel; waiting for a trusted frame
};

enum class Hold : uint8_t { None, WifiBusy, Busy, RadioAway, Gap, OutOfRange };

// Zero in maxChannel or dwellMs reads as the default, so an all-zero Inputs
// (PSRAM .bss) is valid.
struct Inputs {
  uint32_t nowMs = 0;
  Role role = Role::None;
  bool joined = false;       // STA associated
  uint8_t staChannel = 0;    // AP channel while joined (0 unknown)
  uint8_t pinned = 0;        // gSettings.espnowChannel (0 auto; anything outside 1..13 reads as auto)
  uint8_t maxChannel = 11;   // last channel allowed offline (11 until a fleet country is set, Q2; outside 1..13 reads as 11)
  bool sessionUp = false;    // ACTIVE session with the followed (primary) master
  bool busy = false;         // any file transfer, session handshake or key exchange in flight (R5)
  bool wifiBusy = false;     // Wi-Fi connecting, scanning or auto-reconnect retrying
  bool radioAway = false;    // radiopower off, P4 companion offline or restoring
  uint16_t dwellMs = kDwellMs;  // 0 reads as kDwellMs
};

// One frame from the followed master (rank 1) or its named backup (rank 2)
// that passed the trust test (sealed, or group-signed with origin == source,
// not relay-delivered). Never call heard() for anyone else.
struct Heard {
  uint8_t rank = 1;
  bool sealed = false;     // arrived in a SESSION_FRAME (unforgeable, replay-checked)
  uint8_t rxChannel = 0;   // radio channel it arrived on (caller passes the current channel when unknown)
  uint8_t chanInfo = 0;    // heartbeat byte 3; 0 for other frame types
  int8_t rssi = 0;         // rx_ctrl->rssi (older-master candidates: the strongest wins)
  bool heartbeat = true;   // false for other frame types: liveness only, never a lock
};

struct Out {
  uint8_t channel = 0;        // channel to be on now; 0 = no opinion (use espnowch::expected)
  bool sweepSwitch = false;   // `channel` is a sweep dwell: light switch, then call applied()
  bool settle = false;        // a hunt that moved the radio just ended: full apply of `channel`
                              // (or of espnowch::expected when 0) even if the cache says it is there
  bool sendProbe = false;     // send one probe heartbeat to the master now, on the current channel
  bool wifiRetryHeld = false; // hold the Wi-Fi rehunt (live, sealed master session)
  uint8_t persistMasterChannel = 0;  // nonzero once: save it as the last master channel
  Phase phase = Phase::Off;
  Hold hold = Hold::None;
};

// Sweep order: index 0 = the saved channel (when 1..max), then 1..max without it.
inline uint8_t effectiveMaxChannel(uint8_t maxChannel) { return validChannel(maxChannel) ? maxChannel : 11; }
inline uint8_t sweepLength(uint8_t saved, uint8_t maxChannel) {
  (void)saved;  // with or without the saved channel up front, every allowed channel appears once
  return effectiveMaxChannel(maxChannel);
}
inline uint8_t sweepChannelAt(uint8_t idx, uint8_t saved, uint8_t maxChannel) {  // 0 past the end
  const uint8_t maxCh = effectiveMaxChannel(maxChannel);
  if (idx >= maxCh) return 0;
  if (!validChannel(saved) || saved > maxCh) return uint8_t(idx + 1);
  if (idx == 0) return saved;
  return idx < saved ? idx : uint8_t(idx + 1);  // idx-th of 1..max without `saved`
}
inline uint32_t gapAfter(uint8_t failures) {  // kGapMs[min(failures,4)-1]; 0 reads as 1
  return kGapMs[failures <= 1 ? 0 : (failures >= 4 ? 3 : failures - 1)];
}

inline const char* phaseText(Phase p) {
  switch (p) {
    case Phase::Off: return "not following (not a mesh worker with a master)";
    case Phase::Pinned: return "pinned here (espnowchannel)";
    case Phase::Joined: return "joined to Wi-Fi: the AP decides the channel";
    case Phase::WifiPaused: return "paused: Wi-Fi joined on another channel";
    case Phase::Following: return "following the master";
    case Phase::Waiting: return "master silent: waiting for the next sweep";
    case Phase::Sweeping: return "hunting for the master";
    case Phase::Confirming: return "answer heard, confirming";
  }
  return "?";
}
// JSON value for espnowstatus "follow" (Q17: single lowercase words).
inline const char* phaseKey(Phase p) {
  switch (p) {
    case Phase::Off: return "off";
    case Phase::Pinned: return "pinned";
    case Phase::Joined: return "joined";
    case Phase::WifiPaused: return "paused";
    case Phase::Following: return "following";
    case Phase::Waiting: return "waiting";
    case Phase::Sweeping: return "sweeping";
    case Phase::Confirming: return "confirming";
  }
  return "off";
}
inline const char* holdText(Hold h) {
  switch (h) {
    case Hold::None: return "";
    case Hold::WifiBusy: return "Wi-Fi is connecting or scanning";
    case Hold::Busy: return "a file transfer or handshake is running";
    case Hold::RadioAway: return "the radio is off, stalled or its companion is restarting";
    case Hold::Gap: return "waiting between sweeps";
    case Hold::OutOfRange: return "the master's channel is not allowed here without Wi-Fi";
  }
  return "";
}

class Follow {
 public:
  // ESP-NOW start: silence counts from here; a rebooted worker starts on the
  // saved master channel.
  void begin(uint8_t savedMasterChannel, uint32_t nowMs) {
    *this = Follow();
    savedCh_ = masterCh_ = validChannel(savedMasterChannel) ? savedMasterChannel : 0;
    lastHeardMs_ = lastPrimaryMs_ = nowMs;
    lastSealedMs_ = nowMs - kMaxAgeMs;       // sealedEver_ is false as well
    lastRecheckMs_ = nowMs - kRecheckGapMs;  // the first recheck probe may go out at once
    nextSweepAtMs_ = nowMs;
  }

  // Every lap while Sweeping/Confirming, else at least 4 Hz.
  Out tick(const Inputs& in) {
    Out o;
    const uint32_t now = in.nowMs;
    maxCh_ = effectiveMaxChannel(in.maxChannel);
    freshen(now);
    if (in.busy) {  // the busy cap counts per busy spell
      if (!busyHeld_) {
        busyHeld_ = true;
        busySinceMs_ = now;
      }
    } else {
      busyHeld_ = false;
    }

    if (in.role != Role::Worker) {
      endHunt(Phase::Off);
    } else if (validChannel(in.pinned)) {
      endHunt(Phase::Pinned);
      o.channel = in.joined ? 0 : in.pinned;  // the owner's pin (R3), whatever maxChannel says
    } else if (in.joined) {
      const bool differ = validChannel(in.staChannel) && validChannel(masterCh_) && in.staChannel != masterCh_;
      endHunt(differ ? Phase::WifiPaused : Phase::Joined);
    } else {
      autoWorker(in, now, o);
    }

    // Sealed frames from the primary only: a backup's frames, or a replayed
    // group-signed heartbeat, never hold Wi-Fi off.
    // The primary must be live (heard within 30 s) and a sealed frame from it
    // within 45 s: the keep-alive probe at 20 s and its 10 s retry both fit.
    o.wifiRetryHeld = in.role == Role::Worker && in.sessionUp && sealedEver_ &&
                      age(now, lastPrimaryMs_) < kSilenceMs && age(now, lastSealedMs_) < kWifiHoldMs;
    if (in.role == Role::Worker && pendingSave_) {  // held while Off: a master/None device saves nothing
      o.persistMasterChannel = pendingSave_;
      pendingSave_ = 0;
    }
    if (swept_ && !hunting()) {  // the light switches left the parking AP behind: one full apply
      swept_ = false;
      o.settle = true;
    }
    o.phase = phase_;
    o.hold = hold_;
    return o;
  }

  // A trusted frame from the followed master (rank 1) or its backup (rank 2).
  void heard(uint32_t nowMs, const Heard& h) {
    const uint8_t ann = chanOf(h.chanInfo);
    const uint8_t rx = validChannel(h.rxChannel) ? h.rxChannel : 0;
    if (ann != 0 && !h.sealed && ann != rx) {
      // A group-signed claim of another channel than the one it arrived on (an
      // adjacent-channel leak, or a replay): not liveness, and no move. Ask (a
      // sealed answer moves us); otherwise the silence clock runs out and the
      // hunt finds the channel where the claim and the arrival agree.
      recheck_ = true;
      if (phase_ == Phase::Confirming) nextDwell(nowMs);  // the master is not here
      return;
    }
    const bool primarySilent = age(nowMs, lastPrimaryMs_) >= kSilenceMs;  // before this frame
    lastHeardMs_ = nowMs;
    heardEver_ = true;
    if (h.rank == 1) {
      lastPrimaryMs_ = nowMs;
      if (h.sealed) {
        lastSealedMs_ = nowMs;
        sealedEver_ = true;
      }
    }
    const bool mayMove = h.rank == 1 || primarySilent;
    const bool wasHunting = hunting();

    if (ann != 0 && mayMove) {  // sealed, or announced == arrival channel
      adopt(ann);
      if (phase_ == Phase::Waiting || wasHunting) enterFollowing();
      return;
    }
    if (ann == 0 && wasHunting && h.heartbeat && rx != 0 && mayMove &&
        (candCh_ == 0 || h.rssi > candRssi_)) {
      // Older master: it was heard here, but maybe through an adjacent-channel
      // leak. Dwell a little further and keep the strongest.
      candCh_ = rx;
      candRssi_ = h.rssi;
      candLeft_ = kCandidateDwells;
    }
    if (phase_ == Phase::Waiting) {
      enterFollowing();  // liveness on the home channel
    } else if (phase_ == Phase::Confirming) {
      nextDwell(nowMs);  // trusted, but it does not confirm this channel
    }
    // Sweeping: keep sweeping. Following: liveness. Off/Pinned/Joined/WifiPaused:
    // bookkeeping only.
  }

  // The caller matched the last probe: a tagged ACK from the master's radio
  // address whose ackFor is the probe's msgId (1.0).
  void probeAcked(uint32_t nowMs, uint8_t rxChannel) {
    (void)rxChannel;  // the ACK arrives on the radio's channel, which is the dwell channel
    if (phase_ != Phase::Sweeping || !probeSent_) return;
    phase_ = Phase::Confirming;
    dwellStartMs_ = nowMs;  // the confirm window counts from the ACK
  }

  // Result of a sweep switch; nowMs is read AFTER the switch returned, so the
  // dwell counts from the finished switch (covers the Hosted RPC on the P4).
  // ok=false means this channel was refused (country): the next dwell follows.
  // A busy radio (mutex held by a scan) or a stalled switch is not a refusal:
  // call abortHunt() instead.
  void applied(uint32_t nowMs, uint8_t ch, bool ok) {
    if (phase_ != Phase::Sweeping || !asked_ || switched_ || ch != dwellCh_) return;
    if (ok) {
      switched_ = true;
      dwellStartMs_ = nowMs;
    } else {
      nextDwell(nowMs);
    }
  }

  // The hunt cannot use the radio now (switch found the radio mutex held, a
  // Hosted RPC stalled or timed out, a probe send blocked): back to Waiting
  // without counting a failure; the gap keeps the current back-off.
  void abortHunt(uint32_t nowMs, Hold why) {
    if (!hunting()) return;
    toWaiting(nowMs, gapAfter(failures_));
    hold_ = why;
  }

  // Status pieces
  Phase phase() const { return phase_; }
  Hold hold() const { return hold_; }
  uint8_t masterChannel() const { return masterCh_; }  // last trusted master channel, 0 unknown
  bool everHeard() const { return heardEver_; }
  uint32_t silentMs(uint32_t nowMs) const { return age(nowMs, lastHeardMs_); }  // since begin() when never heard
  uint8_t failures() const { return failures_; }  // failed sweeps since the last success (saturating)
  uint32_t nextSweepInMs(uint32_t nowMs) const {
    if (phase_ != Phase::Waiting) return 0;
    const int32_t d = int32_t(nextSweepAtMs_ - nowMs);
    return d > 0 ? uint32_t(d) : 0;
  }
  uint8_t sweepChannel() const {  // the dwell channel while Sweeping/Confirming
    return hunting() ? dwellCh_ : 0;
  }

 private:
  // Age of a past stamp; a stamp slightly in the future (taken after `now`) is 0.
  static uint32_t age(uint32_t now, uint32_t then) {
    const int32_t d = int32_t(now - then);
    return d < 0 ? 0 : uint32_t(d);
  }
  static void slide(uint32_t now, uint32_t& stamp) {
    if (int32_t(now - stamp) > int32_t(kMaxAgeMs)) stamp = now - kMaxAgeMs;
  }
  void freshen(uint32_t now) {
    slide(now, lastHeardMs_);
    slide(now, lastPrimaryMs_);
    slide(now, lastSealedMs_);
    slide(now, lastRecheckMs_);
    slide(now, busySinceMs_);
    slide(now, nextSweepAtMs_);
  }

  bool hunting() const { return phase_ == Phase::Sweeping || phase_ == Phase::Confirming; }
  uint8_t home() const { return (validChannel(masterCh_) && masterCh_ <= maxCh_) ? masterCh_ : 0; }
  Hold gapHold() const { return (validChannel(masterCh_) && !home()) ? Hold::OutOfRange : Hold::Gap; }

  void adopt(uint8_t ch) {
    masterCh_ = ch;
    recheck_ = false;
    if (ch != savedCh_) {
      savedCh_ = ch;
      pendingSave_ = ch;
    }
  }
  void resetDwellFlags() { asked_ = switched_ = probeSent_ = false; }
  void setDwell(uint32_t now) {  // dwell at sweepIdx_; 0 = past the end
    dwellCh_ = sweepChannelAt(sweepIdx_, savedCh_, maxCh_);
    resetDwellFlags();
    dwellStartMs_ = now;
  }
  // Leave the current dwell: lock a candidate whose extra dwells are used up,
  // else go on to the next index (Sweeping).
  void nextDwell(uint32_t now) {
    if (candCh_ != 0) {
      if (candLeft_ == 0) {
        adopt(candCh_);
        enterFollowing();
        return;
      }
      candLeft_--;
    }
    sweepIdx_ = uint8_t(sweepIdx_ + 1);
    setDwell(now);
    phase_ = Phase::Sweeping;
  }
  void endHunt(Phase p) {  // leave any hunt without counting a failure
    phase_ = p;
    hold_ = Hold::None;
    dwellCh_ = 0;
    candCh_ = candLeft_ = 0;
    candRssi_ = 0;
    resetDwellFlags();
  }
  void enterFollowing() {
    endHunt(Phase::Following);
    failures_ = 0;
  }
  void toWaiting(uint32_t now, uint32_t gapMs) {
    endHunt(Phase::Waiting);
    nextSweepAtMs_ = now + gapMs;
  }
  void failSweep(uint32_t now) {
    if (failures_ < 255) failures_++;
    toWaiting(now, gapAfter(failures_));
    hold_ = gapHold();
  }
  Hold blockingHold(const Inputs& in, uint32_t now) const {
    if (in.radioAway) return Hold::RadioAway;
    if (in.wifiBusy) return Hold::WifiBusy;
    if (in.busy && busyHeld_ && age(now, busySinceMs_) < kBusyHoldMaxMs) return Hold::Busy;
    return Hold::None;
  }

  void autoWorker(const Inputs& in, uint32_t now, Out& o) {
    if (phase_ != Phase::Following && phase_ != Phase::Waiting && !hunting()) {
      phase_ = Phase::Following;  // back from Off/Pinned/Joined/WifiPaused; the silence clock kept running
    }
    hold_ = Hold::None;

    if (phase_ == Phase::Following) {
      o.channel = home();
      if (validChannel(masterCh_) && !home()) hold_ = Hold::OutOfRange;
      if (age(now, lastHeardMs_) < kSilenceMs) {
        // Keep-alive: the Wi-Fi hold is wanted (session up, primary heard) but
        // no sealed frame came for 20 s: the master answers a probe sealed.
        const bool wantSealed = in.sessionUp && heardEver_ && age(now, lastPrimaryMs_) < kSilenceMs &&
                                (!sealedEver_ || age(now, lastSealedMs_) >= kSealedRefreshMs);
        if ((recheck_ || wantSealed) && age(now, lastRecheckMs_) >= kRecheckGapMs) {
          o.sendProbe = true;
          recheck_ = false;
          lastRecheckMs_ = now;
        }
        return;
      }
      phase_ = Phase::Waiting;  // the first sweep is due at once
      nextSweepAtMs_ = now;
    }

    if (phase_ == Phase::Waiting) {
      o.channel = home();
      hold_ = blockingHold(in, now);
      if (hold_ != Hold::None) return;
      if (int32_t(now - nextSweepAtMs_) < 0) {
        hold_ = gapHold();
        return;
      }
      phase_ = Phase::Sweeping;
      sweepIdx_ = 0;
      setDwell(now);
    }

    const Hold h = blockingHold(in, now);
    if (h != Hold::None) {  // superseded: abort without counting a failure
      toWaiting(now, gapAfter(failures_));
      hold_ = h;
      o.channel = home();
      return;
    }

    if (phase_ == Phase::Confirming) {
      if (age(now, dwellStartMs_) < kConfirmMs) {
        o.channel = dwellCh_;
        return;
      }
      // No trusted frame: a forged or leaked ACK costs this dwell, not the sweep.
      nextDwell(now);
      if (phase_ != Phase::Sweeping) {
        o.channel = home();
        return;
      }
    }

    // Sweeping
    const uint16_t dwellMs = in.dwellMs ? in.dwellMs : kDwellMs;
    for (;;) {  // each nextDwell() moves sweepIdx_ forward or locks, so this ends within a sweep length
      if (dwellCh_ == 0) {  // past the end
        if (candCh_ != 0) {
          adopt(candCh_);
          enterFollowing();
        } else {
          failSweep(now);
        }
        o.channel = home();
        return;
      }
      o.channel = dwellCh_;
      if (!asked_) {
        asked_ = true;
        swept_ = true;
        dwellStartMs_ = now;
        o.sweepSwitch = true;
        return;
      }
      if (!switched_) {
        if (age(now, dwellStartMs_) < kApplyTimeoutMs) return;
      } else if (!probeSent_) {
        probeSent_ = true;
        o.sendProbe = true;
        return;
      } else if (age(now, dwellStartMs_) < dwellMs) {
        return;
      }
      nextDwell(now);
      if (phase_ != Phase::Sweeping) {
        o.channel = home();
        return;
      }
    }
  }

  // All-zero default state (EXT_RAM_BSS_ATTR is zero-filled). 48 bytes.
  uint8_t masterCh_ = 0, savedCh_ = 0, pendingSave_ = 0, maxCh_ = 0;
  Phase phase_ = Phase::Off;
  Hold hold_ = Hold::None;
  uint8_t sweepIdx_ = 0, dwellCh_ = 0, failures_ = 0, candCh_ = 0, candLeft_ = 0;
  int8_t candRssi_ = 0;
  bool heardEver_ = false, sealedEver_ = false, asked_ = false, switched_ = false, probeSent_ = false,
       recheck_ = false, busyHeld_ = false, swept_ = false;
  uint32_t lastHeardMs_ = 0, lastPrimaryMs_ = 0, lastSealedMs_ = 0, nextSweepAtMs_ = 0, dwellStartMs_ = 0,
           lastRecheckMs_ = 0, busySinceMs_ = 0;
};

}  // namespace espnowfollow

#endif  // SYSTEM_ESPNOW_CHANNEL_FOLLOW_CORE_H
