#pragma once
#include <stdint.h>

// Control-owner policy. A magic is an ACK correlation byte, never a voice-event
// identity. Retire it across close/reopen too: firmware suppresses reuse for 3 s.
struct G2NativeTerminalSession {
  enum class Phase : uint8_t {
    Closed, Opening, Fetching, Syncing, Idle, AwaitChoice, Recording, Finishing, Confirming,
    AwaitConfirm, Submitting, Reply, Closing
  };
  enum class Tx : uint8_t {
    None, Open, List, SessionChange, SwitchResult, NewResult, SyncBegin, SyncEnd, Asr,
    Content, State, Query, Heartbeat, Close
  };
  static constexpr uint32_t AckMs = 3000, HeartbeatMs = 5000, ReuseMs = 3001;
  Phase phase = Phase::Closed;
  Tx pending = Tx::None;
  uint8_t magic = 0, nextMagic = 0;
  uint32_t sentMs = 0, lastTxMs = 0;
  uint32_t retiredAt[256] = {};
  bool used[256] = {};

  uint8_t allocate(uint32_t now) {
    for (unsigned n = 0; n < 255; ++n) {
      if (++nextMagic == 0) ++nextMagic;
      if (!used[nextMagic] || uint32_t(now - retiredAt[nextMagic]) >= ReuseMs) {
        used[nextMagic] = true;
        retiredAt[nextMagic] = now;
        return nextMagic;
      }
    }
    return 0;
  }
  bool acknowledge(uint8_t value, uint32_t now, Tx* completed) {
    if (!value || pending == Tx::None || value != magic) return false;
    *completed = pending;
    retiredAt[magic] = now;
    pending = Tx::None;
    return true;
  }
  void abandon(uint32_t now) {
    if (pending != Tx::None) retiredAt[magic] = now;
    pending = Tx::None;
  }
  bool timedOut(uint32_t now) const {
    return pending != Tx::None && uint32_t(now - sentMs) >= AckMs;
  }
  bool ownsDraft() const {
    return phase == Phase::Recording || phase == Phase::Finishing || phase == Phase::Confirming ||
        phase == Phase::AwaitConfirm || phase == Phase::Submitting;
  }
  // State admission makes repeated START/STOP/CONFIRM idempotent even when
  // distinct legitimate notifications share one firmware magic byte.
  bool voice(uint8_t command) {
    if (command == 1 && phase == Phase::Idle) { phase = Phase::Recording; return true; }
    if ((command == 2 || command == 3) && phase == Phase::Recording) {
      phase = Phase::Finishing; return true;
    }
    if (command == 4 && phase == Phase::AwaitConfirm) {
      phase = Phase::Submitting; return true;
    }
    if (command == 5 && (phase == Phase::Recording || phase == Phase::Finishing ||
                         phase == Phase::Confirming || phase == Phase::AwaitConfirm)) {
      phase = Phase::Idle; return true;
    }
    return false;
  }
  static const char* name(Phase p) {
    switch (p) {
      case Phase::Closed: return "closed";
      case Phase::Opening: return "opening";
      case Phase::Fetching: return "loading-host-session";
      case Phase::AwaitChoice: return "awaiting-choice";
      case Phase::Syncing: return "syncing-recent-history";
      case Phase::Idle: return "idle";
      case Phase::Recording: return "listening";
      case Phase::Finishing: return "transcribing";
      case Phase::Confirming: return "sending-transcript";
      case Phase::AwaitConfirm: return "awaiting-confirmation";
      case Phase::Submitting: return "submitting";
      case Phase::Reply: return "receiving";
      case Phase::Closing: return "closing";
    }
    return "unknown";
  }
};
