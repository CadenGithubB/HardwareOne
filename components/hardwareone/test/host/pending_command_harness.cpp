// Host harness for System_PendingCommand.cpp (S-11, 2026-11-21): the serial
// console and the UART host link hand a command to cmd_exec and keep the main
// loop running. The production file is compiled as is against the doubles in
// the stub folder test_pending_command.py writes; submitCommandAsync is the
// executor boundary, and the test plays cmd_exec by calling the callbacks it
// captured, in the orders the real task can produce.
#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "System_CommandLimits.h"
#include "System_CommandReply.h"
#include "System_PendingCommand.h"  // the copy beside the doubles (-I stubs first)
#include "System_MemUtil.h"         // the allocator double
#include "System_Utils.h"           // the executor double

// --- doubles (declared in the stub headers) ---------------------------------
struct Queued { ExecAsyncCallback cb; void* user; std::string line; };
std::vector<Queued> gQueued;
bool gQueueFull = false;
bool gRequestAllocFails = false;  // submitCommandAsync's own allocation fails
unsigned gSubmitTries = 0;
bool gAllocFails = false;
std::atomic<long> gLiveAllocs{0};
std::vector<std::string> gBroadcasts;

bool submitCommandAsync(const Command& cmd, ExecAsyncCallback cb, void* user) {
  ++gSubmitTries;
  if (gQueueFull || gRequestAllocFails) return false;
  gQueued.push_back({cb, user, cmd.line.c_str()});
  return true;
}
void* ps_alloc(size_t size, AllocPolicy, const char*) {
  if (gAllocFails) return nullptr;
  ++gLiveAllocs;
  return ::operator new(size);
}
void ps_free(void* p) {
  if (!p) return;
  --gLiveAllocs;
  ::operator delete(p);
}
void broadcastOutput(const String& line) { gBroadcasts.push_back(line.c_str()); }
bool commandQueueFull() { return gQueueFull; }

// --- helpers -------------------------------------------------------------------
static Command line(const char* text, const char* user = "alice") {
  Command c;
  c.line = text;
  c.ctx.auth.user = user;
  return c;
}
static void reset() {
  gQueued.clear();
  gQueueFull = false;
  gSubmitTries = 0;
  gAllocFails = false;
  gRequestAllocFails = false;
  gBroadcasts.clear();
}
static void runQueued(size_t i, bool ok, const char* reply) {
  gQueued[i].cb(ok, reply, gQueued[i].user);
}

int main() {
  // A reply arrives: Waiting until cmd_exec publishes it, then Done once, then Idle.
  {
    reset();
    PendingCommand p;
    String out;
    bool ok = true;
    assert(!p.busy() && p.poll(0, out, ok) == PendingCommand::Poll::Idle);
    assert(p.start(line("status"), 1000) && p.busy());
    assert(!p.start(line("other"), 1000));            // one in flight per channel
    assert(p.poll(1000, out, ok) == PendingCommand::Poll::Waiting);
    assert(gQueued.size() == 1 && gQueued[0].line == "status");
    assert(p.poll(1500, out, ok) == PendingCommand::Poll::Waiting);
    runQueued(0, true, "OK: up 5 s");
    assert(p.poll(1501, out, ok) == PendingCommand::Poll::Done);
    assert(ok && std::string(out.c_str()) == "OK: up 5 s");
    assert(!p.busy() && p.poll(1502, out, ok) == PendingCommand::Poll::Idle);
    assert(std::string(p.context().auth.user.c_str()) == "alice");  // for delivery
    assert(gLiveAllocs == 0);
  }
  // A failed command reports ok=false with its own reply.
  {
    reset();
    PendingCommand p;
    String out;
    bool ok = true;
    p.start(line("nosuch"), 0);
    p.poll(0, out, ok);
    runQueued(0, false, "Error: Unknown command");
    assert(p.poll(1, out, ok) == PendingCommand::Poll::Done && !ok);
    assert(std::string(out.c_str()) == "Error: Unknown command" && gLiveAllocs == 0);
  }
  // Refusals before queueing: empty and over-limit lines, with the executor's texts.
  {
    reset();
    PendingCommand p;
    String out;
    bool ok = true;
    p.start(line(""), 0);
    assert(p.poll(0, out, ok) == PendingCommand::Poll::Refused && !ok);
    assert(std::string(out.c_str()) == kCommandReplyEmptyCommand && gSubmitTries == 0);
    std::string big(CMD_INPUT_MAX + 1, 'x');
    p.start(line(big.c_str()), 0);
    assert(p.poll(0, out, ok) == PendingCommand::Poll::Refused);
    assert(std::string(out.c_str()) == "Error: Command exceeds input limit" && gSubmitTries == 0);
    std::string max(CMD_INPUT_MAX, 'x');
    p.start(line(max.c_str()), 0);
    assert(p.poll(0, out, ok) == PendingCommand::Poll::Waiting && gQueued.size() == 1);
    runQueued(0, true, "OK");
    assert(p.poll(0, out, ok) == PendingCommand::Poll::Done && gLiveAllocs == 0);
  }
  // A full queue is retried, at most every kQueueTryEveryMs, for kQueueRetryMs.
  {
    reset();
    PendingCommand p;
    String out;
    bool ok = true;
    gQueueFull = true;
    p.start(line("status"), 10000);
    assert(p.poll(10000, out, ok) == PendingCommand::Poll::Waiting && gSubmitTries == 1);
    assert(p.poll(10005, out, ok) == PendingCommand::Poll::Waiting && gSubmitTries == 1);
    assert(p.poll(10020, out, ok) == PendingCommand::Poll::Waiting && gSubmitTries == 2);
    gQueueFull = false;                                // room again within 2 s
    assert(p.poll(10040, out, ok) == PendingCommand::Poll::Waiting && gQueued.size() == 1);
    runQueued(0, true, "OK");
    assert(p.poll(10041, out, ok) == PendingCommand::Poll::Done && gLiveAllocs == 0);
    // Still full after 2 s: the waiting call's reply text, and nothing queued.
    reset();
    gQueueFull = true;
    p.start(line("status"), 0);
    uint32_t t = 0;
    PendingCommand::Poll r = PendingCommand::Poll::Waiting;
    for (; r == PendingCommand::Poll::Waiting; t += 7) r = p.poll(t, out, ok);
    assert(r == PendingCommand::Poll::Refused && !ok && t >= PendingCommand::kQueueRetryMs);
    assert(std::string(out.c_str()) == "Error: Command queue full. Try again in a few seconds.");
    assert(gSubmitTries <= PendingCommand::kQueueRetryMs / PendingCommand::kQueueTryEveryMs + 2);
    assert(gBroadcasts.back() == "[ERROR] Command queue full - try again" && gLiveAllocs == 0);
    // No memory for the mailbox, or for the executor's request: out of
    // memory at once, never "queue full", and nothing retried.
    reset();
    gAllocFails = true;
    p.start(line("status"), 0);
    assert(p.poll(0, out, ok) == PendingCommand::Poll::Refused);
    assert(std::string(out.c_str()) == kCommandReplyOutOfMemory && gSubmitTries == 0);
    assert(gBroadcasts.back() == "[ERROR] Out of memory - cannot create request");
    reset();
    gRequestAllocFails = true;
    p.start(line("status"), 0);
    assert(p.poll(0, out, ok) == PendingCommand::Poll::Refused);
    assert(std::string(out.c_str()) == kCommandReplyOutOfMemory && gSubmitTries == 1);
    assert(gBroadcasts.back() == "[ERROR] Out of memory - cannot create request" && gLiveAllocs == 0);
  }
  // No reply in 60 s: TimedOut, the channel is free, and the late reply is
  // dropped by cmd_exec, which frees the mailbox.
  {
    reset();
    PendingCommand p;
    String out;
    bool ok = true;
    p.start(line("openwifi"), 0);
    p.poll(0, out, ok);
    assert(p.poll(PendingCommand::kReplyWaitMs - 1, out, ok) == PendingCommand::Poll::Waiting);
    assert(p.poll(PendingCommand::kReplyWaitMs, out, ok) == PendingCommand::Poll::TimedOut && !ok);
    assert(std::string(out.c_str()).rfind("Error: Command timed out after 60 s.", 0) == 0);
    assert(!p.busy() && gLiveAllocs == 1);             // cmd_exec still owns it
    assert(p.start(line("status"), PendingCommand::kReplyWaitMs + 1));  // the next line goes
    runQueued(0, true, "Connected");                   // the late reply: discarded
    assert(gLiveAllocs == 0);
    p.poll(PendingCommand::kReplyWaitMs + 1, out, ok);
    runQueued(1, true, "OK");
    assert(p.poll(PendingCommand::kReplyWaitMs + 2, out, ok) == PendingCommand::Poll::Done);
    assert(std::string(out.c_str()) == "OK" && gLiveAllocs == 0);  // not the stale reply
  }
  // The millis() wrap does not end a wait early.
  {
    reset();
    PendingCommand p;
    String out;
    bool ok = true;
    const uint32_t nearWrap = 0xFFFFFF00u;
    p.start(line("status"), nearWrap);
    p.poll(nearWrap, out, ok);
    assert(p.poll(nearWrap + 1000, out, ok) == PendingCommand::Poll::Waiting);  // wrapped
    runQueued(0, true, "OK");
    assert(p.poll(nearWrap + 1001, out, ok) == PendingCommand::Poll::Done && gLiveAllocs == 0);
  }
  // Race: cmd_exec publishes while the channel gives up at the deadline.
  // Either the reply is delivered or it is dropped; the mailbox is freed once.
  for (int round = 0; round < 2000; ++round) {
    reset();
    PendingCommand p;
    String out;
    bool ok = true;
    p.start(line("status"), 0);
    p.poll(0, out, ok);
    std::thread exec([] { runQueued(0, true, "OK"); });
    const PendingCommand::Poll r = p.poll(PendingCommand::kReplyWaitMs, out, ok);
    exec.join();
    assert(r == PendingCommand::Poll::Done || r == PendingCommand::Poll::TimedOut);
    if (r == PendingCommand::Poll::Done) assert(ok && std::string(out.c_str()) == "OK");
    assert(gLiveAllocs == 0);
  }
  std::puts("pending command: queue, reply, refusals, retry, timeout, late reply and race passed");
  return 0;
}
