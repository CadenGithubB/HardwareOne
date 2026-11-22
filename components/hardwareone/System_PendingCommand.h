// System_PendingCommand.h - one command a main-loop channel hands to cmd_exec
// without stopping the main loop to wait for it.
//
// The serial console and the UART host link used to run each line through
// submitAndExecuteSync: the main loop stood still for up to 2 s (queue room)
// plus 60 s (the reply), and the display, sensor logging, Bluetooth and glasses
// upkeep, the automation scheduler and the OTA probation heartbeat stood still
// with it (owner decision S-11, 2026-11-21). The command itself always ran on
// cmd_exec through executeCommand, like every other transport's; only the wait
// moves.
//
// A channel owns one PendingCommand. start() queues the line and returns at
// once; the channel calls poll() on each pass of its main-loop tick and stops
// reading its own input while busy(). That keeps today's order exactly: one
// command per channel in flight, the next line read only after the reply, a
// command that reads the console itself (openwifi's Ctrl-C) still owns the
// keystrokes while it runs, and the channel's inline verbs (login, logout,
// whoami) never overtake a queued command.
//
// The shared queue is unchanged: a channel holds at most one slot, as its
// waiting call did. What the waiting call did for a full queue (retry for up
// to 2 s) and a slow command (give up after 60 s, let it finish unreported)
// poll() does without blocking, with the same reply texts.
#pragma once

#include <Arduino.h>
#include <stdint.h>

#include "System_CommandTypes.h"

class PendingCommand {
 public:
  // What poll() reports. Every value but Waiting ends the command: `out` and
  // `ok` are set and the object is idle again.
  enum class Poll : uint8_t {
    Idle,       // nothing pending
    Waiting,    // queued or running; keep the channel's input parked
    Done,       // the reply arrived
    Refused,    // never queued: empty or over-limit line, queue full for 2 s
    TimedOut,   // no reply after 60 s; the command may still finish
  };

  // How long a full queue is retried and how long a reply is awaited: the
  // bounds submitAndExecuteSync applied while it blocked the caller.
  static constexpr uint32_t kQueueRetryMs = 2000;
  static constexpr uint32_t kReplyWaitMs = 60000;
  // How often a full queue is tried again within kQueueRetryMs.
  static constexpr uint32_t kQueueTryEveryMs = 20;

  PendingCommand() = default;
  PendingCommand(const PendingCommand&) = delete;
  PendingCommand& operator=(const PendingCommand&) = delete;

  // Takes the command. False (and nothing changes) while one is pending.
  bool start(const Command& cmd, uint32_t nowMs);

  // Advances the pending command; see Poll. Cheap while waiting.
  Poll poll(uint32_t nowMs, String& out, bool& ok);

  bool busy() const { return state_ != State::Idle; }

  // The context the started command carries, for delivering its reply.
  // Meaningful from start() until poll() reports an end, and after that until
  // the next start().
  const CommandContext& context() const { return command_.ctx; }

 private:
  enum class State : uint8_t { Idle, Queuing, Running };
  struct Slot;
  static void onDone(bool ok, const char* result, void* userData);
  static void freeSlot(Slot* slot);
  void finish();

  Command command_;
  State state_ = State::Idle;
  Slot* slot_ = nullptr;
  uint32_t startedMs_ = 0;
  uint32_t queuedMs_ = 0;
  uint32_t lastTryMs_ = 0;
};
