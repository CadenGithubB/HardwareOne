#include "System_MeshPeers.h"

#if ENABLE_ESPNOW

#include <cstring>

namespace MeshPeers {

bool isHealthy(const uint8_t mac[6]) {
  MeshPeerHealth* h = getMeshPeerHealth(mac, false);
  return h && isMeshPeerAlive(h);
}

bool isReachable(const uint8_t mac[6]) {
  MeshPeerHealth* h = getMeshPeerHealth(mac, false);
  return h && isMeshPeerRecentlyActive(h);
}

bool isKnown(const uint8_t mac[6]) {
  return getMeshPeerMeta(mac, false) != nullptr;
}

String displayName(const uint8_t mac[6]) {
  // Prefer user-set friendlyName, fall back to auto-generated name from the
  // metadata table, then the paired-registry name (getEspNowDeviceName does
  // its own meta + devices walk but returns "" when name is empty rather than
  // "Unknown" — we want a guaranteed non-empty result here).
  MeshPeerMeta* m = getMeshPeerMeta(mac, false);
  if (m) {
    if (m->friendlyName[0]) return String(m->friendlyName);
    if (m->name[0])         return String(m->name);
  }
  String regName = getEspNowDeviceName(mac);
  if (regName.length() > 0) return regName;
  return "Unknown";
}

int countHealthy() {
  if (!gMeshPeers) return 0;
  int n = 0;
  for (int i = 0; i < gMeshPeerSlots; i++) {
    if (gMeshPeers[i].isActive && isMeshPeerAlive(&gMeshPeers[i])) n++;
  }
  return n;
}

int countByRoom(const char* room) {
  return countMeshPeerMetaByRoom(room);
}

} // namespace MeshPeers

#endif // ENABLE_ESPNOW
