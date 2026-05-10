// =============================================================================
// BLE_Peers.cpp — runtime peer registry implementation
// =============================================================================

#include "BLE_Peers.h"

#if ENABLE_BLUETOOTH

#include "System_Settings.h"   // gSettings, setSetting, BlePeer settings rows
#include "System_Debug.h"
#include "System_Utils.h"      // RETURN_VALID_IF_VALIDATE_CSTR, parseBoolArg
#include "System_Command.h"    // CommandEntry, ensureDebugBuffer, getDebugBuffer
#include "System_User.h"       // AuthContext (gExecAuthContext for paired-by capture)

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string.h>

// -----------------------------------------------------------------------------
// Storage
// -----------------------------------------------------------------------------

BlePeerData gBlePeerData[BLE_PEER_MAX] = {};

// Registration table — indexed by kind. Slots are nullptr until
// bleRegisterPeer fills them. We keep the specs themselves stable
// (caller-owned, typically a static const) and just store pointers.
static const BlePeerSpec* gPeerByKind[BLE_PEER_MAX] = { nullptr };

// Insertion-ordered list for iteration. Up to BLE_PEER_MAX entries.
static const BlePeerSpec* gPeerInOrder[BLE_PEER_MAX] = { nullptr };
static size_t             gPeerCount               = 0;

// -----------------------------------------------------------------------------
// Built-in metadata-only peers
// -----------------------------------------------------------------------------
// Phone has no owning module yet — when phone integration arrives, this
// registration moves to that module. Until then, BLE_Peers owns the spec
// so the peer appears in `blepeers` and the JSON schema with macCount=1.

static const BlePeerSpec kPhonePeerSpec = {
  BLE_PEER_PHONE,
  "phone",
  "Phone",
  /*macCount=*/1,
  /*connectable=*/false,   // no connect ops yet
  /*ops=*/nullptr,
};

void bleRegisterBuiltinPeers(void) {
  bleRegisterPeer(kPhonePeerSpec);
}

// -----------------------------------------------------------------------------
// Registration
// -----------------------------------------------------------------------------

bool bleRegisterPeer(const BlePeerSpec& spec) {
  if (spec.kind >= BLE_PEER_MAX) {
    DEBUG_G2F("[BLE-Peers] Reject: kind=%u out of range", (unsigned)spec.kind);
    return false;
  }
  if (!spec.name) {
    DEBUG_G2F("[BLE-Peers] Reject: kind=%u name is null", (unsigned)spec.kind);
    return false;
  }
  // Name collision check: a different kind already using this name?
  for (size_t i = 0; i < gPeerCount; i++) {
    if (gPeerInOrder[i] &&
        gPeerInOrder[i]->kind != spec.kind &&
        strcmp(gPeerInOrder[i]->name, spec.name) == 0) {
      DEBUG_G2F("[BLE-Peers] Reject: name '%s' already used by kind=%u",
                spec.name, (unsigned)gPeerInOrder[i]->kind);
      return false;
    }
  }

  const bool firstTime = (gPeerByKind[spec.kind] == nullptr);
  gPeerByKind[spec.kind] = &spec;
  if (firstTime && gPeerCount < BLE_PEER_MAX) {
    gPeerInOrder[gPeerCount++] = &spec;
  }
  DEBUG_G2F("[BLE-Peers] Registered '%s' (kind=%u, %s%s, macCount=%u)",
            spec.name, (unsigned)spec.kind,
            spec.connectable ? "connectable" : "metadata-only",
            firstTime ? "" : ", re-reg",
            (unsigned)spec.macCount);
  return true;
}

bool bleIsPeerRegistered(BlePeerKind kind) {
  return (kind < BLE_PEER_MAX) && (gPeerByKind[kind] != nullptr);
}

const BlePeerSpec* bleFindPeer(BlePeerKind kind) {
  if (kind >= BLE_PEER_MAX) return nullptr;
  return gPeerByKind[kind];
}

const BlePeerSpec* bleFindPeerByName(const char* name) {
  if (!name) return nullptr;
  for (size_t i = 0; i < gPeerCount; i++) {
    if (gPeerInOrder[i] && strcmp(gPeerInOrder[i]->name, name) == 0) {
      return gPeerInOrder[i];
    }
  }
  return nullptr;
}

size_t bleRegisteredPeerCount(void) { return gPeerCount; }

const BlePeerSpec* bleRegisteredPeerAt(size_t i) {
  return (i < gPeerCount) ? gPeerInOrder[i] : nullptr;
}

// -----------------------------------------------------------------------------
// MAC auto-save
// -----------------------------------------------------------------------------

// Stamp the running user's identity onto the peer record if it isn't
// already set. Called from the two natural pairing gestures:
//   * `bleautoconnect <peer> on` (explicit opt-in — strong intent signal)
//   * bleSavePeerMac on first successful connect (catches paths that
//     skip bleautoconnect, e.g. a UI scan-and-pair button)
// Idempotent: once a user owns the peer, re-pairing under a different
// account doesn't silently transfer ownership. To re-assign, clear the
// peer first (e.g. via bondrm or settings edit). This avoids surprise
// privilege swaps if a non-admin briefly handles the device.
static void bleStampPairedByIfBlank(BlePeerKind kind) {
  if (kind >= BLE_PEER_MAX) return;
  BlePeerData& d = gBlePeerData[kind];
  if (d.pairedByUser.length() > 0) return;  // already owned
  extern AuthContext gExecAuthContext;
  const String& who = gExecAuthContext.user;
  if (who.length() == 0) return;             // no current user (e.g. boot reconnect)
  setSetting(d.pairedByUser, who);
  const BlePeerSpec* spec = bleFindPeer(kind);
  DEBUG_G2F("[BLE-Peers] Paired-by user for '%s' set to '%s'",
            spec ? spec->name : "?", who.c_str());
}

void bleSavePeerMac(BlePeerKind kind, const String& mac1, const String& mac2) {
  if (kind >= BLE_PEER_MAX) return;
  // setSetting is a no-op when the value already matches, so calling on
  // every successful connect is cheap (no flash churn).
  if (mac1.length() > 0) setSetting(gBlePeerData[kind].mac1, mac1);
  if (mac2.length() > 0) setSetting(gBlePeerData[kind].mac2, mac2);
  // Capture paired-by identity from whoever ran the connect command (if
  // any). On boot auto-reconnect gExecAuthContext.user is blank and the
  // helper no-ops — the pairedByUser field will already be populated
  // from the original pairing in that case.
  bleStampPairedByIfBlank(kind);
  // Don't auto-flip autoConnect — that's an explicit user opt-in. Pairing
  // saves the MAC; turning on auto-reconnect is a separate gesture.
  const BlePeerSpec* spec = bleFindPeer(kind);
  DEBUG_G2F("[BLE-Peers] Saved MAC for '%s': mac1='%s'%s%s",
            spec ? spec->name : "?",
            mac1.c_str(),
            mac2.length() > 0 ? " mac2='" : "",
            mac2.length() > 0 ? mac2.c_str() : "");
}

// -----------------------------------------------------------------------------
// Boot reconnect
// -----------------------------------------------------------------------------

bool bleAnyPeerWantsAutoConnect(void) {
  // Walk gBlePeerData directly rather than the registry — peer modules
  // typically register from their init function which runs later in boot
  // than the gate check that uses this. The data is loaded by
  // blePeersReadJson during readSettingsJson, well before any peer
  // module init, so this is the earliest reliable signal.
  //
  // Trade-off: we can't filter by `connectable` here because that comes
  // from the spec. In practice it doesn't matter — the only metadata-
  // only peer is "phone" which has no autoConnect persisted, and even if
  // it did, bleBootReconnect would skip it (its ops==nullptr).
  for (size_t i = 0; i < BLE_PEER_MAX; i++) {
    if (gBlePeerData[i].autoConnect && gBlePeerData[i].mac1.length() > 0) {
      return true;
    }
  }
  return false;
}

void bleBootReconnect(void) {
  // Pace 2 s between kicks so the BLE radio isn't doing concurrent scans
  // for two peers. Each peer's connectSaved spawns its own background
  // task — this loop just orders the kick-offs.
  bool anyKicked = false;
  for (size_t i = 0; i < gPeerCount; i++) {
    const BlePeerSpec* p = gPeerInOrder[i];
    if (!p || !p->connectable || !p->ops) continue;
    const BlePeerData& d = gBlePeerData[p->kind];
    if (!d.autoConnect) continue;
    if (d.mac1.length() == 0) {
      DEBUG_G2F("[BLE-Peers] Skip auto-reconnect '%s' — no saved MAC",
                p->name);
      continue;
    }
    if (anyKicked) {
      // Stagger before the next kick.
      vTaskDelay(pdMS_TO_TICKS(2000));
    }
    DEBUG_G2F("[BLE-Peers] Auto-reconnect '%s' (mac1='%s'%s%s)",
              p->name, d.mac1.c_str(),
              d.mac2.length() > 0 ? " mac2='" : "",
              d.mac2.length() > 0 ? d.mac2.c_str() : "");
    if (p->ops->connectSaved) {
      p->ops->connectSaved();
      anyKicked = true;
    }
  }
  if (!anyKicked) {
    DEBUG_G2F("[BLE-Peers] No peers to auto-reconnect");
  }
}

// -----------------------------------------------------------------------------
// CLI: bleautoconnect <peer-name> [on|off]
// -----------------------------------------------------------------------------

const char* cmd_bleautoconnect(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  static char buf[160];

  // Parse "<name> [on|off]". Empty arg → list all peers' state.
  String arg = argsInput;
  arg.trim();
  if (arg.length() == 0) {
    return cmd_blepeers(arg);
  }

  // First token is the peer name; the rest is the optional on/off.
  int sp = arg.indexOf(' ');
  String name = (sp > 0) ? arg.substring(0, sp) : arg;
  String rest = (sp > 0) ? arg.substring(sp + 1) : String();
  rest.trim();

  const BlePeerSpec* p = bleFindPeerByName(name.c_str());
  if (!p) {
    snprintf(buf, sizeof(buf),
             "[BLE] Unknown peer '%s'. Use `blepeers` to list.",
             name.c_str());
    return buf;
  }
  if (!p->connectable) {
    snprintf(buf, sizeof(buf),
             "[BLE] Peer '%s' is metadata-only (no connect implemented)",
             p->name);
    return buf;
  }

  BlePeerData& d = gBlePeerData[p->kind];

  if (rest.length() == 0) {
    snprintf(buf, sizeof(buf),
             "[BLE] %s auto-reconnect: %s (mac1='%s'%s%s)",
             p->displayName ? p->displayName : p->name,
             d.autoConnect ? "enabled" : "disabled",
             d.mac1.length() ? d.mac1.c_str() : "(none)",
             d.mac2.length() ? " mac2='" : "",
             d.mac2.length() ? d.mac2.c_str() : "");
    return buf;
  }

  int on = parseBoolArg(rest);
  if (on < 0) {
    snprintf(buf, sizeof(buf),
             "Usage: bleautoconnect %s [on|off]", p->name);
    return buf;
  }
  setSetting(d.autoConnect, on != 0);
  // Capture paired-by identity at the explicit opt-in moment. This is
  // the strongest pairing-intent signal we have — the user is taking
  // ownership of the peer. The helper no-ops if pairedByUser is
  // already set, so flipping on/off in a session won't churn ownership.
  if (on) bleStampPairedByIfBlank(p->kind);
  snprintf(buf, sizeof(buf),
           "[BLE] %s auto-reconnect %s%s",
           p->displayName ? p->displayName : p->name,
           on ? "enabled" : "disabled",
           on ? " (BT will start in client mode at boot)" : "");
  return buf;
}

// -----------------------------------------------------------------------------
// CLI: blepeers — list every registered peer with state
// -----------------------------------------------------------------------------

// -----------------------------------------------------------------------------
// JSON load/save
// -----------------------------------------------------------------------------
// Settings load runs before peer modules call bleRegisterPeer (the
// modules typically register inside their init function which runs
// after settings load). So serialization can't iterate the registry —
// it walks a hardcoded name↔kind table. This is the single source of
// truth for the JSON schema; if you add a new BlePeerKind, add a row
// here at the same time.
struct PeerJsonEntry {
  BlePeerKind kind;
  const char* name;
};
static const PeerJsonEntry kPeerJsonTable[] = {
  { BLE_PEER_G2_GLASSES, "g2-glasses" },
  { BLE_PEER_R1_RING,    "r1-ring"    },
  { BLE_PEER_PHONE,      "phone"      },
};

void blePeersWriteJson(JsonDocument& doc) {
  // Materialise network.bluetooth.peers as a nested object keyed by peer
  // name. Each value is { mac1, [mac2], autoConnect }.
  JsonObject peers = doc["network"]["bluetooth"]["peers"].to<JsonObject>();
  for (const auto& row : kPeerJsonTable) {
    BlePeerData& d = gBlePeerData[row.kind];
    JsonObject e = peers[row.name].to<JsonObject>();
    e["mac1"] = d.mac1;
    // Only emit mac2 if it has content — keeps single-MAC peers tidy.
    if (d.mac2.length() > 0) e["mac2"] = d.mac2;
    e["autoConnect"] = d.autoConnect;
    // Only emit pairedByUser if set — legacy peers paired before this
    // field existed leave it blank, and callers fall back to
    // firstAdminUser().
    if (d.pairedByUser.length() > 0) e["pairedByUser"] = d.pairedByUser;
  }
}

void blePeersReadJson(JsonDocument& doc) {
  JsonObjectConst peers = doc["network"]["bluetooth"]["peers"].as<JsonObjectConst>();
  if (peers.isNull()) return;
  for (const auto& row : kPeerJsonTable) {
    JsonObjectConst e = peers[row.name].as<JsonObjectConst>();
    if (e.isNull()) continue;
    BlePeerData& d = gBlePeerData[row.kind];
    if (!e["mac1"].isNull())         d.mac1         = e["mac1"].as<const char*>();
    if (!e["mac2"].isNull())         d.mac2         = e["mac2"].as<const char*>();
    if (!e["autoConnect"].isNull())  d.autoConnect  = e["autoConnect"].as<bool>();
    if (!e["pairedByUser"].isNull()) d.pairedByUser = e["pairedByUser"].as<const char*>();
  }
}

const char* cmd_blepeers(const String& /*argsInput*/) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!ensureDebugBuffer()) return "(no buffer)";
  char* out = getDebugBuffer();
  size_t cap = 1024;
  size_t pos = 0;
  pos += snprintf(out + pos, cap - pos, "BLE peers (%u registered):\n",
                  (unsigned)gPeerCount);
  for (size_t i = 0; i < gPeerCount && pos < cap; i++) {
    const BlePeerSpec* p = gPeerInOrder[i];
    if (!p) continue;
    const BlePeerData& d = gBlePeerData[p->kind];
    const bool linked = (p->ops && p->ops->isConnected) ? p->ops->isConnected() : false;
    pos += snprintf(out + pos, cap - pos,
                    "  %-12s %-12s %s auto=%s mac1=%s",
                    p->name,
                    p->displayName ? p->displayName : "",
                    p->connectable ? (linked ? "[CONNECTED]" : "[disconn]")
                                   : "[metadata]",
                    d.autoConnect ? "on" : "off",
                    d.mac1.length() ? d.mac1.c_str() : "(none)");
    if (p->macCount > 1) {
      pos += snprintf(out + pos, cap - pos, " mac2=%s",
                      d.mac2.length() ? d.mac2.c_str() : "(none)");
    }
    pos += snprintf(out + pos, cap - pos, "\n");
  }
  return out;
}

#endif  // ENABLE_BLUETOOTH
