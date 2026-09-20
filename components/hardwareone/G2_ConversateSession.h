#pragma once

#include <stdint.h>

// Dependency-free policy shared by the control owner and host tests. Callers
// serialize access. No BLE, recorder, filesystem or Pi dependency: the native
// session owns the microphone lifetime, not a finite WAV recording.
struct G2ConversateSession {
  enum class Phase : uint8_t { Idle, Armed, Running, Paused, Closing };
  enum class Stop : uint8_t {
    None, User, NativeExit, Deadline, ArmTimeout, AudioStall, AckTimeout,
    Disconnected, Busy, TxFailed, Shutdown, Unsupported, HeartbeatLost, HostAudio
  };
  static constexpr uint32_t HeartbeatMs = 5000;
  static constexpr uint32_t AckTimeoutMs = 12000;
  static constexpr uint32_t AudioTimeoutMs = 8000;
  static constexpr uint32_t ArmTimeoutMs = 60000;
  static constexpr uint32_t CloseTimeoutMs = 3000;
  static constexpr uint32_t MaxHeartbeatFailures = 12;
  // Runtime service availability is independent of a single mic lease. Only
  // a fresh native PREP may arm that lease; idle listening owns no microphone.
  bool enabled = false;
  uint64_t audioExchange = 0; // exact UART stream, zero for packet-only qualification
  uint32_t limitSeconds = 0; // zero = user-ended; only test mode has a deadline
  Phase phase = Phase::Idle;
  Stop reason = Stop::None;
  uint32_t leftGeneration = 0, rightGeneration = 0;
  uint32_t armedMs = 0, startedMs = 0, durationMs = 0;
  uint32_t lastHeartbeatMs = 0, pendingHeartbeatMs = 0;
  uint32_t pendingMagic = 0, heartbeats = 0, acknowledgements = 0;
  uint32_t magicWraps = 0, evenAiDeclines = 0;
  uint32_t heartbeatFailures = 0, consecutiveFailures = 0, ackTimeouts = 0;
  uint32_t closeMs = 0, closeMagic = 0, audioExpectedSinceMs = 0;
  uint32_t packets = 0, lastAudioMs = 0, firstAudioMs = 0, maxGapMs = 0;
  uint32_t lostPackets = 0, duplicates = 0;
  uint8_t lastSequence = 0;
  bool heartbeatPending = false, prepared = false;
  bool started = false, segmentAudio = false;
  bool closeAcknowledged = false, closeTimedOut = false;
  bool transcribeVisible = true, aiCueVisible = true;
  uint32_t interfaceChanges = 0, languageRequests = 0;
  uint32_t replyTokens[8] = {};
  uint8_t replyTokenIndex = 0;
  bool closeAckSafe = true;

  bool reserved(uint32_t magic) const {
    for (auto token : replyTokens) if (token && token == magic) return true;
    return false;
  }
  void reserveReply(uint32_t magic) {
    if (!reserved(magic)) {
      replyTokens[replyTokenIndex] = magic;
      replyTokenIndex = (replyTokenIndex + 1) % 8;
    }
    // COMM_RSP carries no original command. Never let a menu/control ACK
    // masquerade as a heartbeat ACK if the peer reuses its pending token.
    if (heartbeatPending && pendingMagic == magic) {
      heartbeatPending = false;
      ++heartbeatFailures;
      ++consecutiveFailures;
    }
  }

  bool active() const { return phase != Phase::Idle; }
  bool live() const { return active() && phase != Phase::Closing; }
  bool arm(uint32_t now, uint32_t seconds, uint32_t left, uint32_t right) {
    if (active() || (seconds && seconds < 10) || seconds > 3600 || !left || !right) return false;
    const bool keepEnabled = enabled;
    const uint32_t keepLimit = limitSeconds;
    *this = G2ConversateSession{};
    enabled = keepEnabled;
    limitSeconds = keepLimit;
    phase = Phase::Armed;
    armedMs = now;
    lastHeartbeatMs = now;
    durationMs = seconds * 1000;
    leftGeneration = left;
    rightGeneration = right;
    return true;
  }
  bool start(uint32_t now) {
    if (phase != Phase::Armed || !prepared) return false;
    phase = Phase::Running;
    started = true;
    startedMs = audioExpectedSinceMs = now;
    segmentAudio = false;
    return true;
  }
  bool pause() {
    if (phase != Phase::Running && phase != Phase::Armed) return false;
    phase = Phase::Paused;
    segmentAudio = false;
    return true;
  }
  bool resume(uint32_t now) {
    if (phase != Phase::Paused) return false;
    phase = started ? Phase::Running : Phase::Armed;
    audioExpectedSinceMs = now;
    segmentAudio = false;
    return true;
  }
  void audio(uint32_t now, uint32_t generation, uint8_t sequence) {
    if (phase != Phase::Running || generation != leftGeneration) return;
    if (segmentAudio) {
      const uint32_t gap = now - lastAudioMs;
      if (gap > maxGapMs) maxGapMs = gap;
      const uint8_t delta = uint8_t(sequence - lastSequence);
      if (!delta) ++duplicates;
      // At long gaps the 8-bit counter may have wrapped repeatedly. Preserve
      // maxGapMs rather than claiming an exact loss count in that case.
      else if (gap < 1000) lostPackets += delta - 1;
    }
    if (!packets) firstAudioMs = now;
    segmentAudio = true;
    ++packets;
    lastAudioMs = now;
    lastSequence = sequence;
  }
  bool heartbeatDue(uint32_t now) const {
    return live() && prepared && !heartbeatPending &&
           now - lastHeartbeatMs >= HeartbeatMs;
  }
  uint32_t waitMs(uint32_t now) const {
    if (!active()) return 6000;
    if (phase == Phase::Closing) {
      const uint32_t elapsed = now - closeMs;
      return elapsed >= CloseTimeoutMs ? 1 :
          (CloseTimeoutMs - elapsed < 1000 ? CloseTimeoutMs - elapsed : 1000);
    }
    if (!prepared || heartbeatPending) return 1000;
    const uint32_t elapsed = now - lastHeartbeatMs;
    if (elapsed >= HeartbeatMs) return 1;
    const uint32_t remaining = HeartbeatMs - elapsed;
    return remaining < 1000 ? remaining : 1000;
  }
  void heartbeatSent(uint32_t now, uint32_t magic) {
    lastHeartbeatMs = pendingHeartbeatMs = now;
    pendingMagic = magic;
    heartbeatPending = true;
    ++heartbeats;
  }
  bool acknowledge(uint32_t magic, uint32_t error = 0) {
    if (!live() || !heartbeatPending || magic != pendingMagic)
      return false;
    heartbeatPending = false;
    if (error) {
      ++heartbeatFailures;
      ++consecutiveFailures;
    } else {
      ++acknowledgements;
      consecutiveFailures = 0;
    }
    return true;
  }
  Stop check(uint32_t now, bool sameConnections, bool busy) {
    if (!live()) return Stop::None;
    if (!sameConnections) return Stop::Disconnected;
    if (busy) return Stop::Busy;
    if (heartbeatPending && now - pendingHeartbeatMs >= AckTimeoutMs) {
      heartbeatPending = false;
      ++heartbeatFailures;
      ++consecutiveFailures;
      ++ackTimeouts;
    }
    if (consecutiveFailures >= MaxHeartbeatFailures) return Stop::HeartbeatLost;
    if (!started && now - armedMs >= ArmTimeoutMs) return Stop::ArmTimeout;
    if (started && durationMs && now - startedMs >= durationMs) return Stop::Deadline;
    if (phase == Phase::Running &&
        now - (segmentAudio ? lastAudioMs : audioExpectedSinceMs) >= AudioTimeoutMs)
      return Stop::AudioStall;
    return Stop::None;
  }
  void beginClose(uint32_t now, Stop why, uint32_t magic, bool ackSafe = true) {
    phase = Phase::Closing;
    reason = why;
    closeMs = now;
    closeMagic = magic;
    closeAckSafe = ackSafe;
    heartbeatPending = false;
  }
  bool acknowledgeClose(uint32_t magic, uint32_t error) {
    if (phase != Phase::Closing || !closeAckSafe || magic != closeMagic || error) return false;
    closeAcknowledged = true;
    stop(reason);
    return true;
  }
  void stop(Stop why) { phase = Phase::Idle; reason = why; heartbeatPending = false; }
  static const char* phaseName(Phase phase) {
    switch (phase) {
      case Phase::Idle: return "idle";
      case Phase::Armed: return "preparing";
      case Phase::Running: return "running";
      case Phase::Paused: return "paused";
      case Phase::Closing: return "closing";
    }
    return "unknown";
  }
  static const char* stopName(Stop why) {
    switch (why) {
      case Stop::None: return "none";
      case Stop::HostAudio: return "host_audio_failed";
      case Stop::User: return "user";
      case Stop::NativeExit: return "native_exit";
      case Stop::Deadline: return "deadline";
      case Stop::ArmTimeout: return "arm_timeout";
      case Stop::AudioStall: return "audio_stall";
      case Stop::AckTimeout: return "ack_timeout";
      case Stop::Disconnected: return "disconnected";
      case Stop::Busy: return "busy";
      case Stop::TxFailed: return "tx_failed";
      case Stop::Shutdown: return "shutdown";
      case Stop::Unsupported: return "unsupported_native_state";
      case Stop::HeartbeatLost: return "heartbeat_lost";
    }
    return "unknown";
  }
};
