// ESP-NOW multi-hop route table — implementation. See header for the design
// rationale and for why this is distance-vector rather than link-state.

#include "System_ESPNow_Router.h"

#if ENABLE_ESPNOW

#include "System_ESPNow.h"   // gMeshPeers, getMeshPeerHealth, isSelfMac, MESH_PEER_TIMEOUT_MS
#include "System_Debug.h"
#include <esp_now.h>
#include <string.h>

namespace {

// 24 × 20 B = 480 B. Static PSRAM .bss rather than a heap allocation: the table
// is fixed-size, lives for the life of the process, and is touched only from
// espnow_task — none of the reasons to allocate apply.
EXT_RAM_BSS_ATTR MeshRoute       gRoutes[MESH_ROUTE_MAX] = {};
EXT_RAM_BSS_ATTR MeshRelayMetrics gMetrics               = {};
bool gRouterReady = false;

// Combine two path costs. -128 means "no sample", which must NOT poison the
// result — an unknown link tells us nothing, so we fall back to whatever we do
// know. Only when both are unknown is the answer unknown.
inline int8_t metricCombine(int8_t a, int8_t b) {
  if (a == MESH_ROUTE_METRIC_UNKNOWN) return b;
  if (b == MESH_ROUTE_METRIC_UNKNOWN) return a;
  return a < b ? a : b;   // the bottleneck link is the one that matters
}

// MeshPeerHealth::linkRssiEwma is quarter-dB fixed point (see its comment: a
// whole-dB int8 EWMA has a ±3 dB truncation deadband, which would swallow this
// module's entire hysteresis margin). Round to whole dBm for the wire.
inline int8_t linkMetricOf(const MeshPeerHealth* ph) {
  if (!ph || ph->linkRssiEwma == 0) return MESH_ROUTE_METRIC_UNKNOWN;
  int v = (ph->linkRssiEwma + (ph->linkRssiEwma >= 0 ? 2 : -2)) / 4;
  if (v < -127) v = -127;   // keep clear of the -128 sentinel
  if (v > 0)    v = 0;
  return (int8_t)v;
}

// The single definition of "I can hand a frame to this neighbour right now".
// Everything else in this file defers to it, which is what keeps failover and
// fail-back symmetric.
bool neighbourAlive(const uint8_t mac[6], uint32_t nowMs) {
  if (!esp_now_is_peer_exist(mac)) return false;
  const MeshPeerHealth* ph = getMeshPeerHealth(mac, false);
  // everHeardDirect, not lastRxActivityMs != 0: the pairing bootstrap seeds the
  // timestamp optimistically, and installing a direct route on that guess would
  // shadow the real (relayed) path to a peer we cannot actually hear.
  if (!ph || !ph->isActive || !ph->everHeardDirect) return false;
  return (uint32_t)(nowMs - ph->lastRxActivityMs) < MESH_PEER_TIMEOUT_MS;
}

MeshRoute* findRoute(const uint8_t dst[6]) {
  for (int i = 0; i < MESH_ROUTE_MAX; i++) {
    if (gRoutes[i].active && memcmp(gRoutes[i].dst, dst, 6) == 0) return &gRoutes[i];
  }
  return nullptr;
}

MeshRoute* allocRoute() {
  for (int i = 0; i < MESH_ROUTE_MAX; i++) {
    if (!gRoutes[i].active) return &gRoutes[i];
  }
  return nullptr;
}

// How bad a route is, higher = worse. More hops dominates; at equal length the
// weaker bottleneck loses, and an unmeasured path counts as weakest because it
// is the one we know least about. The ×1000 just keeps the hop term above the
// metric term, whose range is [-128, 0].
inline int routeBadness(const MeshRoute& r) {
  const int m = (r.metric == MESH_ROUTE_METRIC_UNKNOWN) ? -128 : (int)r.metric;
  return (int)r.hops * 1000 - m;
}

// Worst learned route in the table, for eviction when full. Direct entries are
// never candidates — losing a neighbour to make room for a 4-hop path would be
// strictly backwards.
MeshRoute* worstLearnedRoute() {
  MeshRoute* worst = nullptr;
  for (int i = 0; i < MESH_ROUTE_MAX; i++) {
    MeshRoute& r = gRoutes[i];
    if (!r.active || r.hops <= 1) continue;
    if (!worst || routeBadness(r) > routeBadness(*worst)) worst = &r;
  }
  return worst;
}

void formatMac(const uint8_t mac[6], char* out, size_t cap) {
  snprintf(out, cap, "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

// Install or refresh a hops == 1 entry for a neighbour we just heard from.
void noteDirect(const uint8_t mac[6], int8_t metric, uint32_t nowMs) {
  MeshRoute* r = findRoute(mac);
  if (r) {
    // A direct path always supersedes a learned one — including when we had
    // been relaying to this node a moment ago and it just came back in range.
    if (r->hops != 1) {
      gMetrics.routesReplaced++;
      DEBUGF(DEBUG_ESPNOW_MESH, "[MESH_ROUTE] %02X:%02X:%02X:%02X:%02X:%02X now DIRECT (was %u hops)",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], (unsigned)r->hops);
    }
    memcpy(r->nextHop, mac, 6);
    r->hops         = 1;
    r->metric       = metric;
    r->lastUpdateMs = nowMs;
    return;
  }
  r = allocRoute();
  if (!r) {
    MeshRoute* victim = worstLearnedRoute();
    if (!victim) return;   // table is all-direct and full: nothing to give up
    victim->active = false;
    r = victim;
  }
  memset(r, 0, sizeof(*r));
  memcpy(r->dst, mac, 6);
  memcpy(r->nextHop, mac, 6);
  r->hops         = 1;
  r->metric       = metric;
  r->lastUpdateMs = nowMs;
  r->active       = true;
  gMetrics.routesInstalled++;
}

}  // namespace

// ---- Lifecycle --------------------------------------------------------------

bool meshRouterInit() {
  if (!gRouterReady) {
    memset(gRoutes, 0, sizeof(gRoutes));
    gRouterReady = true;
  }
  return true;
}

void meshRouterReset() {
  memset(gRoutes, 0, sizeof(gRoutes));
}

// ---- Periodic maintenance ---------------------------------------------------

void meshRouterSyncDirect(uint32_t nowMs) {
  // gMeshPeers is allocated during ESP-NOW init; this can be reached from the
  // mesh tick before/after that window, so do not assume it exists.
  if (!gRouterReady || !gMeshPeers) return;

  // 1. Refresh / install direct entries for every neighbour we can hear.
  for (int i = 0; i < gMeshPeerSlots; i++) {
    const MeshPeerHealth& ph = gMeshPeers[i];
    if (!ph.isActive || isSelfMac(ph.mac)) continue;
    if (!neighbourAlive(ph.mac, nowMs)) continue;
    noteDirect(ph.mac, linkMetricOf(&ph), nowMs);
  }

  // 2. Withdraw direct entries whose peer has gone quiet. This is the failover
  //    trigger: once the hops == 1 route disappears, an advertised multi-hop
  //    path to the same node becomes the best (and only) way to reach it.
  for (int i = 0; i < MESH_ROUTE_MAX; i++) {
    MeshRoute& r = gRoutes[i];
    if (!r.active || r.hops != 1) continue;
    if (neighbourAlive(r.dst, nowMs)) continue;
    char macStr[18]; formatMac(r.dst, macStr, sizeof(macStr));
    DEBUGF(DEBUG_ESPNOW_MESH, "[MESH_ROUTE] direct route to %s withdrawn (peer silent)", macStr);
    r.active = false;
    gMetrics.routesExpired++;
  }

  // 3. Withdraw learned routes whose next hop is no longer usable. Done after
  //    step 2 so a next hop that died in this same pass takes its dependents
  //    with it immediately, instead of leaving a pass-long black hole.
  for (int i = 0; i < MESH_ROUTE_MAX; i++) {
    MeshRoute& r = gRoutes[i];
    if (!r.active || r.hops <= 1) continue;
    const MeshRoute* via = findRoute(r.nextHop);
    if (via && via->active && via->hops == 1) continue;
    char macStr[18]; formatMac(r.dst, macStr, sizeof(macStr));
    DEBUGF(DEBUG_ESPNOW_MESH, "[MESH_ROUTE] route to %s withdrawn (next hop gone)", macStr);
    r.active = false;
    gMetrics.routesExpired++;
  }
}

void meshRouterExpire(uint32_t nowMs) {
  if (!gRouterReady) return;
  for (int i = 0; i < MESH_ROUTE_MAX; i++) {
    MeshRoute& r = gRoutes[i];
    // Direct entries are owned by meshRouterSyncDirect (peer health decides
    // their lifetime); ageing them here too would double-count the withdrawal.
    if (!r.active || r.hops <= 1) continue;
    if ((uint32_t)(nowMs - r.lastUpdateMs) < MESH_ROUTE_EXPIRY_MS) continue;
    char macStr[18]; formatMac(r.dst, macStr, sizeof(macStr));
    DEBUGF(DEBUG_ESPNOW_MESH, "[MESH_ROUTE] route to %s expired (%lu ms stale)",
           macStr, (unsigned long)(nowMs - r.lastUpdateMs));
    r.active = false;
    gMetrics.routesExpired++;
  }
}

// ---- Distance-vector protocol ----------------------------------------------

void meshRouterIngestAdvert(const uint8_t* fromMac, int8_t linkMetric,
                            const V4PayloadPeerListEntry* entries, uint8_t count) {
  if (!gRouterReady || !fromMac || (count > 0 && !entries)) return;
  if (isSelfMac(fromMac)) return;
  // An advert proves the sender is in radio range right now, but we can only
  // USE them as a next hop if the radio can address them. Anything else is a
  // route we would install and never be able to send over.
  if (!esp_now_is_peer_exist(fromMac)) return;

  const uint32_t nowMs = (uint32_t)millis();
  gMetrics.advertsReceived++;

  // Hearing from them IS the freshest possible direct-reachability evidence.
  noteDirect(fromMac, linkMetric, nowMs);

  for (uint8_t i = 0; i < count; i++) {
    const V4PayloadPeerListEntry& e = entries[i];
    if (isSelfMac(e.mac)) continue;                    // our own address, learned back
    if (memcmp(e.mac, fromMac, 6) == 0) continue;      // advertiser listing itself
    if (e.hops == 0) continue;                         // malformed
    const uint8_t newHops = (uint8_t)(e.hops + 1);
    if (newHops > MESH_ROUTE_MAX_HOPS) continue;       // count-to-infinity backstop

    const int8_t newMetric = metricCombine(e.metric, linkMetric);

    MeshRoute* r = findRoute(e.mac);
    if (r) {
      if (r->hops == 1) continue;   // a live direct peer always wins

      if (memcmp(r->nextHop, fromMac, 6) == 0) {
        // Refresh in place from our current next hop — accepted even when the
        // cost got WORSE. That acceptance is what makes DV converge: if the
        // path behind our next hop lengthened, we must hear about it, or we
        // would advertise a stale cost forever.
        r->hops         = newHops;
        r->metric       = newMetric;
        r->lastUpdateMs = nowMs;
        continue;
      }

      const bool shorter   = newHops < r->hops;
      const bool sameButBetter =
          (newHops == r->hops) &&
          (newMetric != MESH_ROUTE_METRIC_UNKNOWN) &&
          (r->metric == MESH_ROUTE_METRIC_UNKNOWN ||
           newMetric > r->metric + MESH_ROUTE_METRIC_HYSTERESIS);
      if (!shorter && !sameButBetter) continue;

      char dstStr[18], viaStr[18];
      formatMac(e.mac, dstStr, sizeof(dstStr));
      formatMac(fromMac, viaStr, sizeof(viaStr));
      DEBUGF(DEBUG_ESPNOW_MESH, "[MESH_ROUTE] %s: %u hops -> %u hops via %s (metric %d)",
             dstStr, (unsigned)r->hops, (unsigned)newHops, viaStr, (int)newMetric);
      memcpy(r->nextHop, fromMac, 6);
      r->hops         = newHops;
      r->metric       = newMetric;
      r->lastUpdateMs = nowMs;
      gMetrics.routesReplaced++;
      continue;
    }

    // New destination.
    r = allocRoute();
    if (!r) {
      MeshRoute* victim = worstLearnedRoute();
      // Only displace something strictly worse than what we are installing.
      if (!victim || victim->hops <= newHops) continue;
      victim->active = false;
      r = victim;
    }
    memset(r, 0, sizeof(*r));
    memcpy(r->dst, e.mac, 6);
    memcpy(r->nextHop, fromMac, 6);
    r->hops         = newHops;
    r->metric       = newMetric;
    r->lastUpdateMs = nowMs;
    r->active       = true;
    gMetrics.routesInstalled++;

    char dstStr[18], viaStr[18];
    formatMac(e.mac, dstStr, sizeof(dstStr));
    formatMac(fromMac, viaStr, sizeof(viaStr));
    DEBUGF(DEBUG_ESPNOW_MESH, "[MESH_ROUTE] learned %s via %s (%u hops, metric %d)",
           dstStr, viaStr, (unsigned)newHops, (int)newMetric);
  }
}

uint8_t meshRouterBuildAdvert(const uint8_t* towardMac,
                              V4PayloadPeerListEntry* out, uint8_t cap) {
  if (!gRouterReady || !out || cap == 0) return 0;
  uint8_t n = 0;
  // Emit shortest-first so that if the table ever outgrows one frame, the
  // routes we drop are the ones our neighbour is least likely to need from us.
  for (uint8_t wantHops = 1; wantHops <= MESH_ROUTE_MAX_HOPS && n < cap; wantHops++) {
    for (int i = 0; i < MESH_ROUTE_MAX && n < cap; i++) {
      const MeshRoute& r = gRoutes[i];
      if (!r.active || r.hops != wantHops) continue;
      if (towardMac) {
        // Split horizon. Both clauses matter: never advertise a route back
        // toward its own next hop, and never tell a node about itself.
        if (memcmp(r.nextHop, towardMac, 6) == 0) continue;
        if (memcmp(r.dst,     towardMac, 6) == 0) continue;
      }
      memcpy(out[n].mac, r.dst, 6);
      out[n].hops   = r.hops;
      out[n].metric = r.metric;
      n++;
    }
  }
  return n;
}

// ---- Forwarding decisions ---------------------------------------------------

bool meshRouterLookup(const uint8_t* dst, uint8_t nextHopOut[6], uint8_t* hopsOut) {
  if (!gRouterReady || !dst) return false;
  const MeshRoute* r = findRoute(dst);
  if (!r) return false;
  if (nextHopOut) memcpy(nextHopOut, r->nextHop, 6);
  if (hopsOut)    *hopsOut = r->hops;
  return true;
}

bool meshRouterIsRouted(const uint8_t* dst) {
  if (!gRouterReady || !dst) return false;
  const MeshRoute* r = findRoute(dst);
  return r && r->hops >= 2;
}

// ---- Introspection ----------------------------------------------------------

int meshRouterCount() {
  int n = 0;
  for (int i = 0; i < MESH_ROUTE_MAX; i++) if (gRoutes[i].active) n++;
  return n;
}

const MeshRoute* meshRouterAt(int index) {
  if (index < 0 || index >= MESH_ROUTE_MAX) return nullptr;
  return gRoutes[index].active ? &gRoutes[index] : nullptr;
}

MeshRelayMetrics& meshRelayMetrics() { return gMetrics; }

void meshRelayMetricsReset() { memset(&gMetrics, 0, sizeof(gMetrics)); }

#endif // ENABLE_ESPNOW
