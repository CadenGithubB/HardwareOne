#pragma once

#include <stddef.h>
#include <stdint.h>

// Start-and-poll state of one web request to the bonded peer's file service
// (/api/bond/fs/list, /stat, /get in WebPage_Bond.cpp). No Arduino, ESP-IDF or
// FreeRTOS dependency, so the host tests run the same transitions.
//
// Why it exists: the three handlers used to send the ESP-NOW request and then
// wait up to 6 s on a semaphore for the reply. esp_http_server is one task, so
// every other web request waited with them. Now the first request starts the
// ESP-NOW request and answers at once with a request id; the page asks the
// same route again with that id until the answer is there.
//
// One Slot per route. Two tasks touch it, always under the bridge mutex of
// WebPage_Bond.cpp: the httpd task (begin, sent, poll, collected) and
// espnow_task, where the reply callback runs (accept). No function here
// blocks, allocates or reads a clock; the caller passes millis().
//
//   Idle ──begin──▶ Starting ──sent(id)──▶ Pending ──accept──▶ Ready
//                      │  └──accept (reply before the send returned)──▶ Ready
//                      └──sent(0): the send failed──▶ Idle
//   Ready ──poll(id)──▶ Collecting ──collected──▶ Idle
//
// Nothing ticks. A request nobody finishes is dropped by the next request
// that looks at the slot: a Pending one after kPendingLimitMs (and its id is
// handed back so the caller frees the protocol's pending entry), an
// uncollected Ready one after kReadyHoldMs. That is also how a browser that
// went away is cleaned up.
namespace bond_fs_poll {

// The protocol answers or reports a timeout itself after 5 s
// (FS_LIST_REQUEST_TIMEOUT_MS), which makes the slot Ready. A request still
// Pending after 7 s means that sweep is not running (ESP-NOW stopped): give
// up, cancel it and tell the page "timeout". Counted from begin, so it
// includes the time the send itself took.
inline constexpr uint32_t kPendingLimitMs = 7000;
// How long a finished answer waits for its page before another request may
// have the slot. The page polls every 250 ms and a background tab is throttled
// to one timer per second, so 2 s is at least one missed poll in either case.
// Until then a new request gets "bridge busy" and retries; a page that was
// reloaded between its start and its poll waits this long for its first
// listing, so the time is kept short.
inline constexpr uint32_t kReadyHoldMs = 2000;

enum class State : uint8_t {
  Idle = 0,    // zero-initialised storage is an idle slot
  Starting,    // the send is running; the request id is not known yet
  Pending,     // sent; waiting for the reply callback
  Ready,       // the answer is stored and waits for its poll
  Collecting,  // the httpd task is sending the stored answer to the page
};

struct Slot {
  State state;
  uint32_t reqId;      // protocol id of the request in the slot; 0 while Starting until known
  uint32_t lastReqId;  // newest protocol id this slot has carried
  uint32_t sinceMs;    // begin time while Starting or Pending; then the time the state was entered
};

inline void release(Slot& s) {
  s.state = State::Idle;
  s.reqId = 0;
}

// Drop what nobody finished. Returns the protocol id the caller must cancel
// (fsListCancel) after it has released the bridge mutex, or 0.
inline uint32_t expire(Slot& s, uint32_t nowMs) {
  const uint32_t age = nowMs - s.sinceMs;  // unsigned: correct across a millis() wrap
  switch (s.state) {
    case State::Pending:
      if (age >= kPendingLimitMs) {
        const uint32_t id = s.reqId;
        release(s);
        return id;
      }
      return 0;
    case State::Ready:
      if (age >= kReadyHoldMs) release(s);
      return 0;
    case State::Starting:
    case State::Collecting:
      // Entered and left inside one handler call. Only a handler that could
      // not take the mutex again leaves one behind; do not let it block the
      // route for good.
      if (age >= kPendingLimitMs) release(s);
      return 0;
    case State::Idle:
      return 0;
  }
  return 0;
}

enum class Begin : uint8_t { Started, Busy };

// A new request wants the slot. Started: the slot is Starting and the caller
// sends now, without the mutex. Busy: another request is in flight or its
// answer is still waiting for its page. cancelId as in expire().
inline Begin begin(Slot& s, uint32_t nowMs, uint32_t& cancelId) {
  cancelId = expire(s, nowMs);
  if (s.state != State::Idle) return Begin::Busy;
  s.state = State::Starting;
  s.reqId = 0;
  s.sinceMs = nowMs;
  return Begin::Started;
}

enum class Sent : uint8_t { Pending, Ready, Failed };

// The send returned reqId (0 = it failed and the protocol keeps no entry).
// Ready: the reply arrived while the send was still returning.
inline Sent sent(Slot& s, uint32_t reqId) {
  if (reqId == 0) {
    release(s);
    return Sent::Failed;
  }
  s.lastReqId = reqId;
  if (s.state == State::Ready && s.reqId == reqId) return Sent::Ready;
  s.state = State::Pending;  // sinceMs stays the begin time
  s.reqId = reqId;
  return Sent::Pending;
}

// A reply for reqId reached the callback. True: the caller copies the payload
// into the bridge before it releases the mutex; the slot is Ready.
//
// While Starting the id of the new request is not known here yet (the protocol
// can deliver the reply before the send call returns). The protocol hands out
// ids from one counter that only goes up, so a reply whose id is not newer
// than the last one this slot carried belongs to an earlier request, and is
// refused.
inline bool accept(Slot& s, uint32_t reqId, uint32_t nowMs) {
  if (reqId == 0) return false;
  if (s.state == State::Pending) {
    if (s.reqId != reqId) return false;
  } else if (s.state == State::Starting) {
    if ((int32_t)(reqId - s.lastReqId) <= 0) return false;
    s.reqId = reqId;
    s.lastReqId = reqId;
  } else {
    return false;
  }
  s.state = State::Ready;
  s.sinceMs = nowMs;
  return true;
}

enum class Poll : uint8_t {
  Pending,   // still waiting; ask again
  Ready,     // the slot is now Collecting: send the stored answer, then collected()
  TimedOut,  // no answer within kPendingLimitMs; cancelId must be cancelled
  Unknown,   // not the request in the slot (never started, dropped or collected)
};

// The page asks for the answer to request id. An answer still in the slot is
// handed to its own poll however late that comes: kReadyHoldMs only decides
// when someone else may have the slot.
inline Poll poll(Slot& s, uint32_t id, uint32_t nowMs, uint32_t& cancelId) {
  cancelId = 0;
  if (id == 0 || s.reqId != id) {
    cancelId = expire(s, nowMs);
    return Poll::Unknown;
  }
  switch (s.state) {
    case State::Pending:
      if ((uint32_t)(nowMs - s.sinceMs) >= kPendingLimitMs) {
        cancelId = s.reqId;
        release(s);
        return Poll::TimedOut;
      }
      return Poll::Pending;
    case State::Ready:
      s.state = State::Collecting;
      s.sinceMs = nowMs;
      return Poll::Ready;
    default:
      return Poll::Unknown;
  }
}

// The answer was copied out or sent.
inline void collected(Slot& s) {
  if (s.state == State::Collecting) release(s);
}

}  // namespace bond_fs_poll
