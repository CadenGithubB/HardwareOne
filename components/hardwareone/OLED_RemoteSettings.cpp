#include "OLED_RemoteSettings.h"

#if ENABLE_OLED_DISPLAY && ENABLE_ESPNOW && ENABLE_BONDED_MODE

#include <ArduinoJson.h>
#include <LittleFS.h>
#include <cstring>
#include <new>
#include "System_VFS.h"
#include "System_Debug.h"

#include "System_BondedPeer.h"
#include "System_ESPNow.h"
#include "System_Mutex.h"
#include "System_Utils.h"
#include "System_MemUtil.h"
#include "OLED_Utils.h"

// ----------------------------------------------------------------------------
// The peer's module set (PSRAM; released by freeRemoteSettingsModules)
// ----------------------------------------------------------------------------
// One value slot per entry. SettingEntry::valuePtr points at the member that
// matches the entry type, so the shared editor reads a peer value exactly like
// a local one.
struct RemoteValueSlot {
  int i = 0;
  bool b = false;
  String s;
};

static SettingsModule*  gRemoteModules = nullptr;
static SettingEntry*    gRemoteEntries = nullptr;
static RemoteValueSlot* gRemoteSlots   = nullptr;
static char*            gRemoteStrings = nullptr;  // key/label/cmdKey/options/group/name/section
static size_t gRemoteModuleCount = 0;
static size_t gRemoteEntryCount = 0;
static size_t gRemoteSlotCount = 0;      // slots constructed (destroyed on release)
static size_t gRemoteStringBytes = 0;
static size_t gRemoteStringUsed = 0;

static const char* gRemoteLastError = "";
static bool gRemoteSchemaMissing = false;  // last load failed because no schema.json is cached
static RemoteSettingsLoadState gLoadState = REMOTE_LOAD_IDLE;
static uint32_t gSyncStartMs = 0;

// The one commit in flight. Its reply is the next CMD_RESP from the bonded peer
// after `seqBefore` (EspNowCmdResult sequence).
struct RemotePending {
  bool active = false;
  uint32_t seqBefore = 0;
  uint32_t sentMs = 0;
  const SettingEntry* entry = nullptr;
  String value;
};
static RemotePending gPending;

static const uint32_t REMOTE_SET_REPLY_TIMEOUT_MS = 5000;  // peer executes on cmd_exec; typical reply << 1 s
static const uint32_t REMOTE_SCHEMA_SYNC_MS       = 6000;  // same budget as the web sync endpoint

// ----------------------------------------------------------------------------
// Schema walk
// ----------------------------------------------------------------------------
// A schema row the OLED editor can change on the peer: a type the editor edits
// (int/bool/string), a setter command, not read-only, not secret, and exposed by
// the bonded-edit policy. Rows without a setter are never offered: the settings
// owner's contract is that clients send the schema's command, never one derived
// from the key.
static bool remoteEntryEditable(JsonObjectConst e, const char* moduleName) {
  const char* type = e["type"] | "";
  if (strcmp(type, "int") != 0 && strcmp(type, "bool") != 0 && strcmp(type, "string") != 0) {
    return false;
  }
  const char* key = e["key"] | "";
  const char* cmdKey = e["cmdKey"] | "";
  if (!key[0] || !cmdKey[0]) return false;
  if ((e["readOnly"] | false) || (e["secret"] | false)) return false;
  return BondedPeer::remoteEditAllowed(moduleName, key);
}

// Visit the exposed modules (at most MAX_SETTINGS_MODULES, each with at least
// one editable row) and their editable rows in schema order. Counting and
// building both walk through here, so the sizes they see always agree.
template <typename ModuleFn, typename EntryFn>
static void remoteSchemaWalk(JsonArrayConst modules, ModuleFn onModule, EntryFn onEntry) {
  size_t exposed = 0;
  for (JsonObjectConst m : modules) {
    if (exposed >= MAX_SETTINGS_MODULES) break;
    const char* name = m["name"] | "";
    if (!name[0] || !BondedPeer::remoteEditAllowed(name, nullptr)) continue;
    JsonArrayConst entries = m["entries"].as<JsonArrayConst>();
    size_t editable = 0;
    for (JsonObjectConst e : entries) {
      if (remoteEntryEditable(e, name)) editable++;
    }
    if (editable == 0) continue;
    onModule(m, editable);
    for (JsonObjectConst e : entries) {
      if (remoteEntryEditable(e, name)) onEntry(e);
    }
    exposed++;
  }
}

// Arena bytes for one optional string: empty/absent strings are stored as nullptr.
static size_t arenaLen(const char* s) { return (s && s[0]) ? strlen(s) + 1 : 0; }

static const char* arenaDup(const char* s) {
  const size_t n = arenaLen(s);
  if (n == 0 || !gRemoteStrings || gRemoteStringUsed + n > gRemoteStringBytes) return nullptr;
  char* out = gRemoteStrings + gRemoteStringUsed;
  memcpy(out, s, n);
  gRemoteStringUsed += n;
  return out;
}

static void remoteSchemaCount(JsonArrayConst modules, size_t& moduleCount,
                              size_t& entryCount, size_t& stringBytes) {
  moduleCount = entryCount = stringBytes = 0;
  remoteSchemaWalk(modules,
    [&](JsonObjectConst m, size_t) {
      moduleCount++;
      stringBytes += arenaLen(m["name"] | "") + arenaLen(m["section"] | "");
    },
    [&](JsonObjectConst e) {
      entryCount++;
      stringBytes += arenaLen(e["key"] | "") + arenaLen(e["label"] | "") +
                     arenaLen(e["cmdKey"] | "") + arenaLen(e["options"] | "") +
                     arenaLen(e["group"] | "");
    });
}

// Fill the allocated arrays from the schema. Ranges and options are the
// schema's; a bool gets the slider's 0..1 range (the schema sends none); the
// schema default is the value until remoteFillValues finds the cached one.
static void remoteSchemaBuild(JsonArrayConst modules) {
  size_t mi = 0, ei = 0;
  remoteSchemaWalk(modules,
    [&](JsonObjectConst m, size_t editable) {
      SettingsModule& mod = gRemoteModules[mi++];
      mod.name = arenaDup(m["name"] | "");
      mod.jsonSection = arenaDup(m["section"] | "");
      mod.entries = &gRemoteEntries[ei];
      mod.count = editable;
      mod.isConnected = nullptr;
      mod.description = "Remote settings";
    },
    [&](JsonObjectConst e) {
      SettingEntry& entry = gRemoteEntries[ei];
      RemoteValueSlot& slot = gRemoteSlots[ei];
      ei++;
      entry.jsonKey = arenaDup(e["key"] | "");
      const char* label = arenaDup(e["label"] | "");
      entry.label = label ? label : entry.jsonKey;
      entry.cmdKey = arenaDup(e["cmdKey"] | "");
      entry.options = arenaDup(e["options"] | "");
      entry.group = arenaDup(e["group"] | "");
      entry.isSecret = false;
      entry.readOnly = false;
      const char* type = e["type"] | "";
      if (strcmp(type, "bool") == 0) {
        entry.type = SETTING_BOOL;
        entry.valuePtr = &slot.b;
        entry.minVal = 0;
        entry.maxVal = 1;
        slot.b = e["default"] | false;
        entry.intDefault = slot.b ? 1 : 0;
      } else if (strcmp(type, "int") == 0) {
        entry.type = SETTING_INT;
        entry.valuePtr = &slot.i;
        entry.minVal = e["min"] | 0;
        entry.maxVal = e["max"] | 0;
        slot.i = e["default"] | 0;
        entry.intDefault = slot.i;
      } else {
        entry.type = SETTING_STRING;
        entry.valuePtr = &slot.s;
        slot.s = e["default"] | "";
      }
    });
  gRemoteModuleCount = mi;
  gRemoteEntryCount = ei;
}

// Read a dotted path ("hardware.sensors.oled") under `root`; null on any miss.
static JsonVariantConst remoteJsonPath(JsonVariantConst root, const char* path) {
  JsonVariantConst cur = root;
  const char* seg = path;
  while (seg && *seg) {
    const char* dot = strchr(seg, '.');
    const size_t len = dot ? (size_t)(dot - seg) : strlen(seg);
    char name[64];
    if (len == 0 || len >= sizeof(name)) return JsonVariantConst();
    memcpy(name, seg, len);
    name[len] = '\0';
    cur = cur[name];
    if (cur.isNull()) return cur;
    seg = dot ? dot + 1 : nullptr;
  }
  return cur;
}

// Copy the peer's current values from its cached settings.json, nested as
// <section>[.<group>].<key> (the layout readRegisteredSettings reads). Missing
// or mistyped values keep the schema default.
static void remoteFillValues(JsonVariantConst root) {
  for (size_t m = 0; m < gRemoteModuleCount; m++) {
    const SettingsModule& mod = gRemoteModules[m];
    JsonVariantConst section = remoteJsonPath(root, mod.jsonSection);
    if (section.isNull()) continue;
    for (size_t i = 0; i < mod.count; i++) {
      const SettingEntry& e = mod.entries[i];
      JsonVariantConst holder = e.group ? section[e.group] : section;
      if (holder.isNull()) continue;
      JsonVariantConst v = holder[e.jsonKey];
      if (v.isNull()) continue;
      switch (e.type) {
        case SETTING_INT:
          if (v.is<int>()) *(int*)e.valuePtr = v.as<int>();
          break;
        case SETTING_BOOL:
          if (v.is<bool>()) *(bool*)e.valuePtr = v.as<bool>();
          break;
        case SETTING_STRING:
          if (v.is<const char*>()) *(String*)e.valuePtr = v.as<const char*>();
          break;
        default:
          break;
      }
    }
  }
}

// Release the module set (load state untouched). A pending commit points into
// the set, so it is dropped with it.
static void releaseRemoteModules() {
  gPending = RemotePending();
  for (size_t i = 0; i < gRemoteSlotCount; i++) gRemoteSlots[i].~RemoteValueSlot();
  free(gRemoteSlots);    // ps_calloc'd — free(), not delete[]
  free(gRemoteEntries);
  free(gRemoteModules);
  free(gRemoteStrings);
  gRemoteSlots = nullptr;
  gRemoteEntries = nullptr;
  gRemoteModules = nullptr;
  gRemoteStrings = nullptr;
  gRemoteModuleCount = gRemoteEntryCount = gRemoteSlotCount = 0;
  gRemoteStringBytes = gRemoteStringUsed = 0;
}

/**
 * Build the peer's editable module set from the cached schema, then copy the
 * current values from the cached settings. The two documents are parsed one
 * after the other, never alive together.
 */
bool loadRemoteSettingsModules() {
  releaseRemoteModules();
  gRemoteSchemaMissing = false;

  if (!BondedPeer::isPaired() || !BondedPeer::isMaster()) {
    gRemoteLastError = "Not bonded master";
    return false;
  }

  {
    String schema = BondedPeer::readCachedSchemaJson();
    if (schema.length() == 0) {
      gRemoteSchemaMissing = true;
      gRemoteLastError = "No cached schema";
      return false;
    }

    // Keep only what the editor uses (drops description/connected/set).
    PSRAM_JSON_DOC(filter);
    JsonObject fm = filter["modules"][0].to<JsonObject>();
    fm["name"] = true;
    fm["section"] = true;
    JsonObject fe = fm["entries"][0].to<JsonObject>();
    static const char* const kEntryFields[] = {
      "key", "label", "type", "min", "max", "options", "cmdKey",
      "readOnly", "secret", "group", "default"};
    for (const char* field : kEntryFields) fe[field] = true;

    PSRAM_JSON_DOC(doc);
    DeserializationError err =
        deserializeJson(doc, schema, DeserializationOption::Filter(filter));
    if (err) {
      DEBUGF(DEBUG_ESPNOW_ROUTER, "[RemoteSettings] schema parse error: %s", err.c_str());
      gRemoteLastError = "Schema parse error";
      return false;
    }
    JsonArrayConst modules = doc["modules"].as<JsonArrayConst>();

    size_t moduleCount = 0, entryCount = 0, stringBytes = 0;
    remoteSchemaCount(modules, moduleCount, entryCount, stringBytes);
    if (entryCount == 0) {
      gRemoteLastError = "No editable settings";
      return false;
    }

    gRemoteModules = (SettingsModule*)ps_calloc(moduleCount, sizeof(SettingsModule),
                                                AllocPolicy::PreferPSRAM, "oled.remote.modules");
    gRemoteEntries = (SettingEntry*)ps_calloc(entryCount, sizeof(SettingEntry),
                                              AllocPolicy::PreferPSRAM, "oled.remote.entries");
    gRemoteSlots = (RemoteValueSlot*)ps_calloc(entryCount, sizeof(RemoteValueSlot),
                                               AllocPolicy::PreferPSRAM, "oled.remote.values");
    gRemoteStrings = (char*)ps_alloc(stringBytes, AllocPolicy::PreferPSRAM, "oled.remote.strings");
    if (!gRemoteModules || !gRemoteEntries || !gRemoteSlots || !gRemoteStrings) {
      releaseRemoteModules();
      DEBUGF(DEBUG_ESPNOW_ROUTER, "[RemoteSettings] alloc failed (%zu mods, %zu entries)",
             moduleCount, entryCount);
      gRemoteLastError = "Out of memory";
      return false;
    }
    for (size_t i = 0; i < moduleCount; i++) new (&gRemoteModules[i]) SettingsModule();
    for (size_t i = 0; i < entryCount; i++) {
      new (&gRemoteEntries[i]) SettingEntry();
      new (&gRemoteSlots[i]) RemoteValueSlot();
    }
    gRemoteSlotCount = entryCount;
    gRemoteStringBytes = stringBytes;
    gRemoteStringUsed = 0;
    remoteSchemaBuild(modules);
  }

  {
    String settings = BondedPeer::readCachedSettingsJson();
    if (settings.length() > 0) {
      PSRAM_JSON_DOC(doc);
      if (!deserializeJson(doc, settings)) remoteFillValues(doc.as<JsonVariantConst>());
    }
  }

  gRemoteLastError = "";
  DEBUGF(DEBUG_ESPNOW_ROUTER, "[RemoteSettings] Loaded %zu modules, %zu entries",
         gRemoteModuleCount, gRemoteEntryCount);
  return true;
}

RemoteSettingsLoadState remoteSettingsLoadStep() {
  switch (gLoadState) {
    case REMOTE_LOAD_IDLE:
      if (loadRemoteSettingsModules()) {
        gLoadState = REMOTE_LOAD_READY;
      } else if (!gRemoteSchemaMissing) {
        gLoadState = REMOTE_LOAD_FAILED;
      } else if (!BondedPeer::beginSchemaSync()) {
        // The master's sync tick never fetches the schema itself, so the screen
        // asks once; a precondition failure (offline, not master) ends here.
        gRemoteLastError = BondedPeer::lastError();
        gLoadState = REMOTE_LOAD_FAILED;
      } else {
        gSyncStartMs = millis();
        gLoadState = REMOTE_LOAD_SYNCING;
      }
      break;
    case REMOTE_LOAD_SYNCING:
      if (BondedPeer::schemaSyncDone()) {
        gLoadState = loadRemoteSettingsModules() ? REMOTE_LOAD_READY : REMOTE_LOAD_FAILED;
      } else if (millis() - gSyncStartMs >= REMOTE_SCHEMA_SYNC_MS) {
        gRemoteLastError = "Schema sync timed out";
        gLoadState = REMOTE_LOAD_FAILED;
      }
      break;
    case REMOTE_LOAD_READY:
    case REMOTE_LOAD_FAILED:
      break;
  }
  return gLoadState;
}

const char* remoteSettingsLastError() { return gRemoteLastError; }

/**
 * Free the peer's module set, drop the pending commit, reset the load state.
 */
void freeRemoteSettingsModules() {
  releaseRemoteModules();
  gLoadState = REMOTE_LOAD_IDLE;
  gRemoteSchemaMissing = false;
  gRemoteLastError = "";
}

/**
 * Get remote settings modules
 */
const SettingsModule** getRemoteSettingsModules(size_t& count) {
  static const SettingsModule* modulePointers[MAX_SETTINGS_MODULES];
  
  // Clamp BOTH the reported count and the fill. These used to disagree: count
  // was the peer's raw gRemoteModuleCount while the loop stopped at
  // MAX_SETTINGS_MODULES, so a peer advertising more modules than we can hold
  // would hand callers a count covering never-written .bss slots — which they
  // would then dereference as SettingsModule* and call isConnected() through.
  count = (gRemoteModuleCount < MAX_SETTINGS_MODULES) ? gRemoteModuleCount
                                                      : MAX_SETTINGS_MODULES;
  for (size_t i = 0; i < count; i++) {
    modulePointers[i] = &gRemoteModules[i];
  }
  
  return modulePointers;
}

/**
 * Send the entry's schema setter to the bonded peer. The `remote:` path applies
 * the OLED user's authorization to the inner command and the bond session gate;
 * its "sent" result only means the frame left, so the commit stays pending
 * until the peer's CMD_RESP (remoteSettingsPollOutcome).
 */
bool applyRemoteSettingChange(const SettingEntry* entry, const String& value, String& errOut) {
  errOut = "";
  if (!entry || !entry->cmdKey || !entry->cmdKey[0]) {
    errOut = "No command";
    return false;
  }
  String trimmed = value;
  trimmed.trim();
  if (trimmed.length() == 0) {
    errOut = "Empty value";
    return false;
  }
  if (gPending.active) {
    errOut = "Busy";
    return false;
  }

  const uint32_t seqBefore = espnowCmdResultSeq();
  char out[128];
  out[0] = '\0';
  const String cmd = String("remote:") + entry->cmdKey + " " + trimmed;
  if (!executeOLEDCommandWithResult(cmd, out, sizeof(out)) || strncmp(out, "Error", 5) == 0) {
    errOut = out[0] ? String(out) : String("Send failed");
    DEBUGF(DEBUG_ESPNOW_ROUTER, "[RemoteSettings] %s not sent: %s", entry->cmdKey, errOut.c_str());
    return false;
  }

  gPending.active = true;
  gPending.seqBefore = seqBefore;
  gPending.sentMs = millis();
  gPending.entry = entry;
  gPending.value = trimmed;
  return true;
}

// Mirror an applied value into the entry's cached slot so the list shows it.
static void remoteSlotStore(const SettingEntry* entry, const String& value) {
  if (!entry || !entry->valuePtr) return;
  switch (entry->type) {
    case SETTING_INT:    *(int*)entry->valuePtr = atoi(value.c_str()); break;
    case SETTING_BOOL:   *(bool*)entry->valuePtr = (value == "1" || value == "true"); break;
    case SETTING_STRING: *(String*)entry->valuePtr = value; break;
    default: break;
  }
}

RemoteSettingsOutcome remoteSettingsPollOutcome(String& textOut) {
  textOut = "";
  if (!gPending.active) return REMOTE_OUTCOME_NONE;

  EspNowCmdResult r;
  if (espnowCmdResultGet(r) && r.seq != gPending.seqBefore) {
    uint8_t peer[6];
    if (BondedPeer::peerMacBytes(peer) && macEqual6(r.srcMac, peer)) {
      gPending.active = false;
      textOut = r.text;
      // Same rule as the local string save: a reply that starts with "Error"
      // is a failure even when the peer's executor reported success.
      if (r.success && strncmp(r.text, "Error", 5) != 0) {
        remoteSlotStore(gPending.entry, gPending.value);
        return REMOTE_OUTCOME_APPLIED;
      }
      return REMOTE_OUTCOME_FAILED;
    }
    gPending.seqBefore = r.seq;  // another peer's reply; keep waiting for ours
  }

  if (millis() - gPending.sentMs >= REMOTE_SET_REPLY_TIMEOUT_MS) {
    gPending.active = false;
    return REMOTE_OUTCOME_TIMEOUT;
  }
  return REMOTE_OUTCOME_PENDING;
}

#endif // ENABLE_OLED_DISPLAY && ENABLE_ESPNOW && ENABLE_BONDED_MODE
