// System_ESPNow_Router.h — multi-hop route table + relay policy for the ESP-NOW mesh.
//
// WHAT THIS ADDS
// --------------
// Before this module the mesh was a single-hop star: `ttl` was stamped on every
// frame and never read, and a peer you could not hear directly was simply
// unreachable. Two mechanisms turn that into a real mesh, and they are
// deliberately separate because they solve different problems:
//
//   1. FLOOD RELAY (broadcast-class traffic — TEXT, TIME_SYNC).
//      A verified, non-duplicate BROADCAST_AUTH frame with ttl >= 2 is
//      re-tagged with the mesh group key, ttl-1, and re-emitted to every
//      neighbour except the one it came from. No routing state is needed:
//      gV4Dedup already keys on `origin`, so each node acts on a given message
//      exactly once no matter how many copies reach it. Loop suppression is the
//      dedup table; ttl is the belt-and-braces bound.
//
//   2. ROUTED UNICAST (everything addressed to one peer — CMD, TEXT, the
//      KEY_EX/SESSION handshakes themselves).
//      Flooding unicast traffic would be absurd, so nodes learn paths with a
//      compact split-horizon distance-vector protocol (PEER_LIST, this file)
//      and forward frames hop-by-hop inside a RELAY_DATA envelope.
//
// WHY DISTANCE VECTOR
// -------------------
// The fleet is tens of nodes on one radio channel, not thousands. DV converges
// in O(diameter) advertisement rounds, costs one 250-byte frame per neighbour
// per 30 s, and needs no link-state database — the whole table is 24 × 20 B.
// Its classic weakness (count-to-infinity) is closed three ways here: strict
// split horizon (a route is never advertised back toward its own next hop —
// cheap because the transport already unicasts one advert per neighbour),
// a hard MESH_ROUTE_MAX_HOPS ceiling, and expiry that outruns the advertisement
// period by 3×.
//
// THE ONE RULE WORTH REMEMBERING
// ------------------------------
// A direct radio peer that is currently ALIVE always wins over any learned
// route. Conversely a direct peer that has gone silent stops being a route at
// all, which is what lets a pair that drifts out of range fail over to a relay
// path automatically — and fail back when they can hear each other again.
//
// Threading: every entry point here runs on espnow_task (RX drain + the mesh
// tick). Nothing takes a lock; do not call these from another task.

#ifndef SYSTEM_ESPNOW_ROUTER_H
#define SYSTEM_ESPNOW_ROUTER_H

#include "System_BuildConfig.h"

#if ENABLE_ESPNOW

#include <Arduino.h>
#include "System_ESPNow_Wire.h"

// ---- Tunables ---------------------------------------------------------------

// Route slots. Direct peers cap at MESH_PEER_MAX (16); the surplus is far peers.
#define MESH_ROUTE_MAX               24
// Hard ceiling on path length. Also the count-to-infinity backstop: a route can
// never be installed at a cost above this, so a poisoned loop dies in a few
// rounds instead of ramping forever. 8 hops at ~100 m/hop is far past any
// plausible deployment of this hardware.
#define MESH_ROUTE_MAX_HOPS          8
// A route with no refresh in this long is dropped. Must be a comfortable
// multiple of the advertisement period so one lost frame never drops a good
// path (3× here: two adverts may be missed).
#define MESH_ROUTE_EXPIRY_MS         90000UL
// How often we advertise our table to each neighbour.
#define MESH_ROUTE_ADVERT_INTERVAL_MS 30000UL
// An equal-hop alternative must beat the incumbent by this many dB before we
// switch to it. Without hysteresis two equally-good paths flap on RSSI noise,
// and every flap resets in-flight sessions' effective path.
#define MESH_ROUTE_METRIC_HYSTERESIS 3

// "No RSSI sample" sentinel, matching the convention used by MeshPeerHealth.
#define MESH_ROUTE_METRIC_UNKNOWN    ((int8_t)-128)

// ---- Route table ------------------------------------------------------------

struct MeshRoute {
  uint8_t  dst[6];         // destination MAC
  uint8_t  nextHop[6];     // neighbour to hand the frame to (== dst when hops == 1)
  uint8_t  hops;           // 1 = direct radio peer, >= 2 = learned
  int8_t   metric;         // worst link RSSI along the path (dBm); -128 = unknown
  uint32_t lastUpdateMs;   // millis() of last refresh — drives expiry
  bool     active;
};

// ---- Lifecycle --------------------------------------------------------------

// Allocates the table (PSRAM) and clears it. Safe to call twice.
bool meshRouterInit();
// Drops every learned route and every direct entry. Used by `espnowmeshroutes
// clear` and on ESP-NOW shutdown.
void meshRouterReset();

// ---- Periodic maintenance (call from the mesh tick) -------------------------

// Re-derive the hops == 1 entries from live peer health, and withdraw any
// learned route whose next hop is no longer a usable neighbour. This is what
// makes the table self-healing: it is the only place direct reachability is
// decided, so "alive" is defined once.
void meshRouterSyncDirect(uint32_t nowMs);

// Age out routes not refreshed within MESH_ROUTE_EXPIRY_MS. Direct entries are
// exempt — meshRouterSyncDirect owns their lifetime.
void meshRouterExpire(uint32_t nowMs);

// ---- Distance-vector protocol ----------------------------------------------

// Fold one neighbour's advertisement into our table. `linkMetric` is OUR
// measured RSSI for the frame that carried it (the last hop's quality), so the
// installed metric is min(advertised bottleneck, this link).
void meshRouterIngestAdvert(const uint8_t* fromMac, int8_t linkMetric,
                            const V4PayloadPeerListEntry* entries, uint8_t count);

// Serialize our table for one neighbour, applying split horizon: routes whose
// next hop IS that neighbour are omitted, because telling a node "I can reach X"
// when our only path to X runs through that same node is how DV loops form.
// Returns the number of entries written (<= cap).
uint8_t meshRouterBuildAdvert(const uint8_t* towardMac,
                              V4PayloadPeerListEntry* out, uint8_t cap);

// ---- Forwarding decisions ---------------------------------------------------

// Resolve the next hop for `dst`. Returns false when we have no path — the
// caller then falls back to a direct send (correct for a freshly-paired peer we
// have no telemetry for yet). `hopsOut` may be null.
bool meshRouterLookup(const uint8_t* dst, uint8_t nextHopOut[6], uint8_t* hopsOut);

// True when `dst` is reachable ONLY through a relay, i.e. a routed send is
// required. Callers that must respect the smaller routed payload budget
// (ESPNOW_V4_RELAY_MAX_INNER_PAYLOAD) ask this first.
bool meshRouterIsRouted(const uint8_t* dst);

// ---- Introspection (CLI / JSON) --------------------------------------------

int              meshRouterCount();
const MeshRoute* meshRouterAt(int index);

// ---- Relay + routing counters ----------------------------------------------
//
// Kept here rather than in RouterMetrics so the whole multi-hop story is
// readable in one place. `espnowmeshmetrics` prints these; `espnowresetstats`
// zeroes them.
struct MeshRelayMetrics {
  // Flood relay (broadcast class)
  uint32_t floodForwards;      // frames we re-emitted
  uint32_t floodFanout;        // individual neighbour sends those became
  uint32_t floodFanoutFails;   // esp_now_send rejections during fan-out
  uint32_t floodDropTtl;       // eligible but ttl exhausted
  uint32_t floodDropRate;      // refused by the rate limiter
  uint32_t floodDropNoKey;     // no group key for the frame's mesh
  uint32_t floodDupSuppressed; // duplicates the dedup table absorbed
  uint32_t selfOriginDrops;    // frames that came back to their originator

  // Routed unicast (RELAY_DATA)
  uint32_t routedTx;           // frames we originated over a route
  uint32_t routedForwards;     // envelopes we forwarded for someone else
  uint32_t routedDelivered;    // envelopes addressed to us, unwrapped locally
  uint32_t routedDropNoRoute;  // no next hop for finalDst
  uint32_t routedDropTtl;      // envelope ttl exhausted mid-path
  uint32_t routedDropTooBig;   // frame did not fit an envelope
  uint32_t routedDropPolicy;   // inner opcode not relay-eligible / nested envelope
  uint32_t rxViaRelay;         // accepted frames whose origin != radio sender —
                               // the single best "multi-hop is actually working" gauge

  // Route table churn
  uint32_t routesInstalled;
  uint32_t routesReplaced;
  uint32_t routesExpired;
  uint32_t advertsSent;
  uint32_t advertsReceived;
};

MeshRelayMetrics& meshRelayMetrics();
void              meshRelayMetricsReset();

#endif // ENABLE_ESPNOW
#endif // SYSTEM_ESPNOW_ROUTER_H
