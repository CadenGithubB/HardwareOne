// System_PendingCommand.cpp - see System_PendingCommand.h.
#include "System_PendingCommand.h"

#include <atomic>
#include <new>

#include "System_CommandLimits.h"
#include "System_CommandReply.h"
#include "System_Debug.h"
#include "System_MemUtil.h"
#include "System_Utils.h"  // submitCommandAsync, broadcastOutput

// The reply's mailbox. cmd_exec fills it in onDone; the channel's main-loop
// tick reads it in poll(). Whichever side lets go last frees it: the channel
// after reading a finished reply, cmd_exec when the channel stopped waiting.
struct PendingCommand::Slot {
  enum : uint8_t { kRunning, kFinished, kAbandoned };
  std::atomic<uint8_t> state{kRunning};
  bool ok = false;
  String out;
};

void PendingCommand::freeSlot(Slot* slot) {
  slot->~Slot();
  ps_free(slot);
}

bool PendingCommand::start(const Command& cmd, uint32_t nowMs) {
  if (state_ != State::Idle) return false;
  command_ = cmd;
  state_ = State::Queuing;
  startedMs_ = nowMs;
  lastTryMs_ = nowMs - kQueueTryEveryMs;  // the first poll() tries at once
  return true;
}

// cmd_exec task. Must not block: it only copies the reply and publishes it.
void PendingCommand::onDone(bool ok, const char* result, void* userData) {
  Slot* slot = static_cast<Slot*>(userData);
  slot->ok = ok;
  slot->out = result ? result : "";
  uint8_t expected = Slot::kRunning;
  if (slot->state.compare_exchange_strong(expected, Slot::kFinished,
                                          std::memory_order_acq_rel)) {
    return;  // the channel reads it on its next pass
  }
  // The channel gave up waiting (kAbandoned): the reply goes nowhere, as a
  // late reply after submitAndExecuteSync's timeout always did.
  DEBUG_CMD_FLOWF("[pending] reply after the channel stopped waiting; discarded");
  freeSlot(slot);
}

void PendingCommand::finish() {
  state_ = State::Idle;
  slot_ = nullptr;
  // command_ keeps its context for context() until the next start().
}

PendingCommand::Poll PendingCommand::poll(uint32_t nowMs, String& out, bool& ok) {
  ok = false;
  if (state_ == State::Idle) return Poll::Idle;

  if (state_ == State::Queuing) {
    // The same refusals submitAndExecuteSync made before queueing.
    const size_t length = command_.line.length();
    if (!commandInputLengthAccepted(length)) {
      if (length == 0) {
        out = kCommandReplyEmptyCommand;
        broadcastOutput("[ERROR] Empty command");
      } else {
        out = "Error: Command exceeds input limit";
        broadcastOutput("[ERROR] Command exceeds input limit");
      }
      finish();
      return Poll::Refused;
    }
    // A full queue is retried, at most every kQueueTryEveryMs: each try
    // allocates and frees the request, so a busy executor must not turn the
    // main loop into an allocation loop.
    if (nowMs - lastTryMs_ < kQueueTryEveryMs) return Poll::Waiting;
    lastTryMs_ = nowMs;
    void* memory = ps_alloc(sizeof(Slot), AllocPolicy::PreferPsram, "cmd.pending");
    Slot* slot = memory ? new (memory) Slot() : nullptr;
    if (slot && submitCommandAsync(command_, &PendingCommand::onDone, slot)) {
      slot_ = slot;
      state_ = State::Running;
      queuedMs_ = nowMs;
      return Poll::Waiting;
    }
    if (slot) freeSlot(slot);
    // Only a full queue is worth waiting for; no memory for the mailbox or
    // the request is answered at once, as submitAndExecuteSync did.
    if (!slot || !commandQueueFull()) {
      broadcastOutput("[ERROR] Out of memory - cannot create request");
      out = kCommandReplyOutOfMemory;
      finish();
      return Poll::Refused;
    }
    if (nowMs - startedMs_ < kQueueRetryMs) return Poll::Waiting;
    broadcastOutput("[ERROR] Command queue full - try again");
    out = "Error: Command queue full. Try again in a few seconds.";
    finish();
    return Poll::Refused;
  }

  // Running: the reply, if cmd_exec has published it.
  if (slot_->state.load(std::memory_order_acquire) == Slot::kFinished) {
    out = slot_->out;
    ok = slot_->ok;
    freeSlot(slot_);
    finish();
    return Poll::Done;
  }
  if (nowMs - queuedMs_ < kReplyWaitMs) return Poll::Waiting;
  uint8_t expected = Slot::kRunning;
  if (!slot_->state.compare_exchange_strong(expected, Slot::kAbandoned,
                                            std::memory_order_acq_rel)) {
    // It finished at the deadline: deliver it after all.
    out = slot_->out;
    ok = slot_->ok;
    freeSlot(slot_);
    finish();
    return Poll::Done;
  }
  // cmd_exec now owns the slot and frees it when the command returns.
  ERROR_COMMANDF("command timed out after %lu ms; command execution may continue",
                 static_cast<unsigned long>(kReplyWaitMs));
  out = "Error: Command timed out after 60 s. It may still finish; check the result before "
        "repeating it.";
  finish();
  return Poll::TimedOut;
}
