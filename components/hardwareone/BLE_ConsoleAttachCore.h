#pragma once
// The app console on the phone pipe's link (G2 client mode): which link may hold
// a console slot, and when that slot is made or ended.
// - Server mode (openble): the console owns the host and advertises; every app
//   central that connects gets a slot, as before.
// - Client mode: the command service is registered on the fresh client host,
//   before any scan or connect, and never advertised. Bluedroid reports every
//   link (both temples, the ring, the phone) to it; only the link of the
//   authenticated, READY phone pipe becomes a slot, made by reconcile() and
//   never by a connect. The slot is found by the pipe's live connection id
//   (the GATT client and server share one per link), never by the phone's
//   rotating address, and is keyed by the pipe generation, so a reused id
//   never inherits a session. A slot is a session waiting for the app until
//   the app joins it (its first write the console admits): only then does it
//   count as a connected app (connection state, events, status words).
// Contract with the pipe's link record: it is published only while the pipe's
// own client holds the link, and may stay set for a moment after the link
// drops; linkChanged() covers that. Bluedroid tells the server of a drop before
// the client, so a record can also be published just after a drop that races
// READY: a slot made then sits on a dead id until the next link event on that
// id ends it. That event comes first: a new link needs a setup turn, and the
// pipe holds its turn until READY (BLE_SetupWorker.h).
// Pure: the caller holds its lifecycle lock, applies the plan and stores
// next, so the host test runs this file as is. See docs/PHONE_CONSOLE_VS_PIPE.md.
#include <cstdint>

namespace hw1console {

// Off: no console server. Server: openble owns the host.
// ClientRegistered: the command service sits on the G2 client host.
enum class Mode : uint8_t { Off, Server, ClientRegistered };

// A copy of the pipe's link record: valid while the pipe is authenticated and
// READY. generation is BLE_PhoneGatt's count of links, one per link.
struct PipeLink {
  bool valid = false;
  uint32_t generation = 0;
  uint16_t connId = 0;
};

// The console slot on the phone pipe's link.
struct Slot {
  bool present = false;
  uint32_t generation = 0;  // the pipe generation it was made for
  uint16_t connId = 0;      // that generation's link
  bool joined = false;      // the app has written to it; a new slot has not
};

// The same slot: present, generation and link (joined is its state, not its identity).
inline bool sameSlot(const Slot& a, const Slot& b) {
  return a.present == b.present && (!a.present || (a.generation == b.generation && a.connId == b.connId));
}

// What the console keeps between decisions, under the caller's lock.
struct Attach {
  Slot slot;
  // A pipe generation whose link has gone: never attached again, even while
  // the pipe's record of it is still set.
  bool closed = false;
  uint32_t closedGeneration = 0;
};

struct Inputs {
  Mode mode = Mode::Off;
  bool enabled = false;  // the console setting is on
  bool secret = false;   // blesecret is set: client mode serves Secure Channel only
  PipeLink pipe;
};

// What the caller does, in this order, before storing next.
struct Plan {
  bool server = false;  // a link event in server mode: the server path, as before
  bool clear = false;   // end the slot's session: epoch, secure channel, sign-in
  bool create = false;  // then make the slot on the pipe's link, typed Phone
  bool join = false;    // the app joined the slot: it now counts (state, connect event)
  Attach next;
};

// The console is registered on the client host and its setting is on.
inline bool attachable(Mode mode, bool enabled) { return mode == Mode::ClientRegistered && enabled; }

// A link came up or went down on connId. Either way the link that held that id
// before has gone: its slot ends, and the pipe generation it carried is never
// attached again (connId reuse never inherits a slot). In client mode nothing
// is ever admitted here; reconcile() makes the slot.
inline Plan linkChanged(Mode mode, uint16_t connId, const PipeLink& pipe, const Attach& now) {
  Plan p;
  p.server = mode == Mode::Server;
  p.next = now;
  if (now.slot.present && now.slot.connId == connId) {
    p.clear = true;
    p.next.slot = Slot{};
    p.next.closed = true;
    p.next.closedGeneration = now.slot.generation;
  }
  if (pipe.valid && pipe.connId == connId) {
    p.next.closed = true;
    p.next.closedGeneration = pipe.generation;
  }
  return p;
}

// ServerCallbacks::onConnect: server mode admits as before; client mode never.
inline Plan onConnect(Mode mode, uint16_t connId, const PipeLink& pipe, const Attach& now) {
  return linkChanged(mode, connId, pipe, now);
}

// ServerCallbacks::onDisconnect: in client mode only the slot on that id ends;
// a temple or ring dropping changes no count, event or connection state.
inline Plan onDisconnect(Mode mode, uint16_t connId, const PipeLink& pipe, const Attach& now) {
  return linkChanged(mode, connId, pipe, now);
}

// The slot follows the pipe: made for the pipe's link while client mode is
// registered, enabled and has a secret, and the record is valid and not closed;
// ended when any of those goes or the generation or connection id changes. A
// changed link gets a new slot (a new session), never the old one.
inline Plan reconcile(const Inputs& in, const Attach& now) {
  Plan p;
  p.next = now;
  const bool live = attachable(in.mode, in.enabled) && in.secret && in.pipe.valid &&
                    !(now.closed && now.closedGeneration == in.pipe.generation);
  const Slot want{live, live ? in.pipe.generation : 0u, live ? in.pipe.connId : uint16_t(0)};
  if (sameSlot(now.slot, want)) return p;
  p.clear = now.slot.present;
  p.create = live;
  p.next.slot = want;
  return p;
}

// A write the console admitted on connId (the write path, after reconcile):
// the first one on the slot's link joins it. Any other id, no slot, or a slot
// already joined changes nothing.
inline Plan join(const Attach& now, uint16_t connId) {
  Plan p;
  p.next = now;
  if (!now.slot.present || now.slot.connId != connId || now.slot.joined) return p;
  p.join = true;
  p.next.slot.joined = true;
  return p;
}

// bledisconnect on the slot's link: a joined session ends (epoch, secure
// channel, sign-in) and the pipe keeps the link, so the next reconcile makes a
// fresh slot there, waiting for the app again. Another id, no slot, or one no
// app has joined (nothing to disconnect) changes nothing.
inline Plan endSession(const Attach& now, uint16_t connId) {
  Plan p;
  p.next = now;
  if (!now.slot.present || !now.slot.joined || now.slot.connId != connId) return p;
  p.clear = true;
  p.next.slot = Slot{};
  return p;
}

}  // namespace hw1console
