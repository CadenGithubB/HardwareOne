// ============================================================================
// System_DebugFlags.h — the debug-flag X-macro table (single source of truth)
// ============================================================================
// Include-order-dependent FRAGMENT of System_Debug.h: it needs DEBUG_BIT and
// DebugFlagMask already defined, so nothing may include it directly.
//
// One row per flag generates, in this file:
//   DbgBank / DbgFlagIdx        — dense uint8_t indices for table lookups
//                                 (DebugFlagMask has no integral conversion,
//                                 so a mask can never subscript an array)
//   DEBUG_<SYM>                 — the 256-bit mask constants, same spellings
//                                 — these spellings are frozen API: call
//                                 sites all over the component depend on them
//   kDbg* parallel columns      — bit / parent / bank / tag / mask, indexed
//                                 by DbgFlagIdx
// and, in the consumers:
//   the settings→flag map       — System_Settings.cpp expands the
//                                 settingsField column into DBG_MAP rows
//   the parent-recompute tables — System_Debug.cpp expands settingsField,
//                                 DBG_SUBBOOL_LIST and DBG_AGG_FAMILY_LIST
//                                 into dbgRecomputeParent()'s columns
//   the settings registry       — System_Settings.cpp expands the cmdIdent/
//                                 group/jsonKey/label columns into the debug
//                                 SettingEntry row pools (row ORDER lives in
//                                 the pick list there, not in these tables)
// plus the static_asserts at the bottom that hold the invariants, so a bad
// row is a build error, not a runtime mystery.
//
// Everything generated here is `inline constexpr` — bare `constexpr` at
// namespace scope has internal linkage, i.e. one .rodata copy per TU across
// ~143 TUs, and the app partition cannot afford that.

#ifndef DEBUG_BIT
#error "System_DebugFlags.h is a fragment of System_Debug.h — include System_Debug.h instead"
#endif

// ---------------------------------------------------------------------------
// Banks — one byte-aligned bank per family (16 bits for the fast growers),
// so a printed hex mask reads family-per-byte. Bits 240-247 are one spare
// whole bank (deliberately unnamed) for a future sensor/subsystem.
// ---------------------------------------------------------------------------
//   F(sym, base, width, "Label")
#define DBG_BANK_LIST(F) \
  F(CORE,          0, 24, "Core")         \
  F(MEMORY,       24,  8, "Memory")       \
  F(ESPNOW,       32, 16, "ESP-NOW")      \
  F(MQTT,         48,  8, "MQTT")         \
  F(AUTO,         56,  8, "Automations")  \
  F(BT,           64,  8, "Bluetooth")    \
  F(G2,           72, 16, "G2")           \
  F(SR,           88,  8, "Speech")       \
  F(LLM,          96,  8, "LLM")          \
  F(MAPS,        104,  8, "Maps")         \
  F(CAMERA,      112,  8, "Camera")       \
  F(I2C,         120,  8, "I2C")          \
  F(GPS,         128,  8, "GPS")          \
  F(RTC,         136,  8, "RTC")          \
  F(IMU,         144,  8, "IMU")          \
  F(THERMAL,     152,  8, "Thermal")      \
  F(TOF,         160,  8, "ToF")          \
  F(INPUT,       168,  8, "Input")        \
  F(APDS,        176,  8, "APDS")         \
  F(PRESENCE,    184,  8, "Presence")     \
  F(FMRADIO,     192,  8, "FM Radio")     \
  F(MICROPHONE,  200,  8, "Microphone")   \
  F(ANO_ENCODER, 208,  8, "ANO Encoder")  \
  F(UART,        216,  8, "UART")         \
  F(RING,        224, 16, "R1 Ring")      \
  F(CONTROL,     248,  8, "Control")

enum DbgBank : uint8_t {
#define DBG_BANK_X(sym, base, width, label) DBG_BANK_##sym,
  DBG_BANK_LIST(DBG_BANK_X)
#undef DBG_BANK_X
  DBG_BANK_COUNT
};

#define DBG_BANK_X(sym, base, width, label) base,
inline constexpr uint8_t kDbgBankBase[DBG_BANK_COUNT] = { DBG_BANK_LIST(DBG_BANK_X) };
#undef DBG_BANK_X

#define DBG_BANK_X(sym, base, width, label) width,
inline constexpr uint8_t kDbgBankWidth[DBG_BANK_COUNT] = { DBG_BANK_LIST(DBG_BANK_X) };
#undef DBG_BANK_X

#define DBG_BANK_X(sym, base, width, label) label,
inline constexpr const char* kDbgBankLabel[DBG_BANK_COUNT] = { DBG_BANK_LIST(DBG_BANK_X) };
#undef DBG_BANK_X

// ============================================================================
// Debug flag map — 256 bits, one 8-bit bank per family (16 for the fast
// growers), each bank starting on a byte boundary so a printed hex mask
// reads family-per-byte. Spare bits belong to the bank they sit in — add
// new sub-flags there instead of appending at the end.
//
// Bit positions are internal-only: settings persist as per-flag booleans
// and are rebuilt by name in System_Settings.cpp, so renumbering only
// invalidates raw `log start flags=0x...` masks.
//
// Sub-flag convention: parent bit first, subs behind it. The parent is the
// master switch; DEBUG_*F macros gate on parent-OR-sub. parentBit records
// the gating that actually exists (PARENT|SUB enqueues), NOT the naming —
// rows whose macros pass a bare sub-bit (ESPNOW_*, AUTO_*, BLUETOOTH_*,
// LLM_*, MAPS_*, SR_WAKE/COMMAND/TUNING) have no parent link (255).
//
// The top bank (bits 248-255) is CONTROL: bits that modify how a message is
// gated rather than naming a producer. Only bit 255 is assigned. Control
// rows carry an empty TAG — getDebugCategoryName() skips them so the tag
// resolves to the real producer riding alongside.
// ============================================================================
// The settingsField column carries a marker token, not conditional
// compilation: DBG_NO_SETTING is never #defined — consumers pattern-match it
// with DBG_SF_IS_NONE and emit NOTHING for such rows, so the sentinel costs
// zero code and zero data. Classic two-level CHECK/PROBE: the probe expands
// inside a second macro invocation, where the rescan re-splits its arguments.
#define DBG_PP_CAT(a, b)   DBG_PP_CAT_I(a, b)
#define DBG_PP_CAT_I(a, b) a##b
#define DBG_PP_2ND(a, b, ...) b
#define DBG_PP_CHECK(...)  DBG_PP_2ND(__VA_ARGS__, 0, ~)
#define DBG_SF_NONE_PROBE_DBG_NO_SETTING ~, 1,
#define DBG_SF_IS_NONE(field) DBG_PP_CHECK(DBG_PP_CAT(DBG_SF_NONE_PROBE_, field))  // 1 for DBG_NO_SETTING, else 0
// Same CHECK/PROBE trick for the cmdIdent column's DBG_NO_CMD sentinel (the one
// ALWAYS control row): the CLI-thunk generator emits no command for it.
#define DBG_CMD_NONE_PROBE_DBG_NO_CMD ~, 1,
#define DBG_CMD_IS_NONE(cmd) DBG_PP_CHECK(DBG_PP_CAT(DBG_CMD_NONE_PROBE_, cmd))  // 1 for DBG_NO_CMD, else 0

//   X(SYM, bit, BANK, parentBit, "TAG", settingsField, cmdIdent, "group", "jsonKey", "label")
//     parentBit     — bit number of the family master switch this sub rides
//                     behind in PARENT|SUB enqueues; 255 = none (root row)
//     TAG           — writer-side category string (getDebugCategoryName);
//                     empty only for control bits
//     settingsField — the gSettings bool this bit is rebuilt from at boot
//                     (the settings→flag map in System_Settings.cpp), or
//                     DBG_NO_SETTING for control bits with no persisted form.
//                     One field per row; a flag raised by a SECOND field is
//                     recorded in DBG_FLAG_EXTRA_SETTINGS below.
//     cmdIdent      — CLI command name as a bare token; the registry consumer
//                     stringizes it at its FIRST macro level (C1 paste rule),
//                     so it is never re-scanned. DBG_NO_CMD (never #defined,
//                     never expanded — a dead placeholder like DBG_NO_SETTING)
//                     on the one control row.
//     group/jsonKey/label — settings-registry strings (System_Settings.cpp):
//                     (group, jsonKey) is the JSON nesting identity under
//                     "system.debug"; label is the UI string. VERBATIM
//                     transcriptions of the pre-C2 hand rows — NEVER computed
//                     from SYM (NTP ↔ group "datetime", DISPLAY ↔ group
//                     "oled"): renaming one renames persisted debug.json
//                     keys. Empty ("") only on the control row, which emits
//                     no registry row. Registry ORDER is a separate fact (UI
//                     card order ≠ bit order) and lives in the pick list in
//                     System_Settings.cpp, not here.
#define DBG_FLAG_LIST(X) \
  /* ---- Word 0 (bits 0-63): core system ----------------------------------- */                                                                                                                                                                       \
  /* Bits 0-15: core singles (bank full — new subsystems get their own bank)  */                                                                                                                                                                       \
  X(AUTH,                    0, CORE,       255, "AUTH",            debugAuth,               debugauth,                 "authentication", "enabled",    "All Authentication")                                                                          \
  /* bit 1 free (was DEBUG_SECURITY — removed pre-1.0: never gated on anywhere) */                                                                                                                                                                     \
  X(HTTP,                    2, CORE,       255, "HTTP",            debugHttp,               debughttp,                 "http",           "enabled",    "All HTTP") /* firmware's own request/handler logs */                                          \
  /* HTTPS: ESP-IDF TLS/server tag verbosity (esp-tls-mbedtls,                */                                                                                                                                                                       \
  /* esp_https_server, httpd*) via applyHttpsLogLevels(). OFF silences the    */                                                                                                                                                                       \
  /* benign handshake-reject/conn-reset flood browsers produce against a      */                                                                                                                                                                       \
  /* self-signed cert; ON surfaces full TLS handshake detail.                 */                                                                                                                                                                       \
  X(HTTPS,                   3, CORE,       255, "HTTPS",           debugHttps,              debughttps,                "https",          "enabled",    "All HTTPS/TLS")                                                                               \
  X(SSE,                     4, CORE,       255, "SSE",             debugSse,                debugsse,                  "sse",            "enabled",    "All SSE")                                                                                     \
  X(CLI,                     5, CORE,       255, "CLI",             debugCli,                debugcli,                  "cli",            "enabled",    "All CLI")                                                                                     \
  X(CMD_FLOW,                6, CORE,       255, "CMD_FLOW",        debugCommandFlow,        debugcommandflow,          "commands",       "enabled",    "All Commands")                                                                                \
  X(COMMAND_SYSTEM,          7, CORE,       255, "CMD_SYS",         debugCommandSystem,      debugcommandsystem,        "commands",       "system",     "System") /* modular command registry operations */                                            \
  X(USERS,                   8, CORE,       255, "USERS",           debugUsers,              debugusers,                "users",          "enabled",    "All Users")                                                                                   \
  X(SYSTEM,                  9, CORE,       255, "SYSTEM",          debugSystem,             debugsystem,               "system",         "enabled",    "All System")                                                                                  \
  X(STORAGE,                10, CORE,       255, "STORAGE",         debugStorage,            debugstorage,              "storage",        "enabled",    "All Storage") /* file operations */                                                           \
  X(LOGGER,                 11, CORE,       255, "LOGGER",          debugLogger,             debuglogger,               "logger",         "enabled",    "Enabled") /* sensor logger internals */                                                       \
  X(PERFORMANCE,            12, CORE,       255, "PERF",            debugPerformance,        debugperformance,          "performance",    "enabled",    "All Performance")                                                                             \
  X(WIFI,                   13, CORE,       255, "WIFI",            debugWifi,               debugwifi,                 "wifi",           "enabled",    "All WiFi")                                                                                    \
  X(NTP,                    14, CORE,       255, "NTP",             debugDateTime,           debugdatetime,             "datetime",       "enabled",    "All NTP/DateTime") /* NTP sync, setup, anchors, timestamp resolution */                       \
  X(DISPLAY,                15, CORE,       255, "DISPLAY",         debugDisplay,            debugdisplay,              "oled",           "enabled",    "All OLED") /* OLED init/probe/boot-animation/mode-transitions */                              \
  /* NOTIFICATIONS: notification pipeline diagnostics — ring lag/skips,       */                                                                                                                                                                       \
  /* stale/cooldown drops, SSE toast-queue saturation (periodic [NOTIF]       */                                                                                                                                                                       \
  /* line + `notifstats` CLI)                                                 */                                                                                                                                                                       \
  X(NOTIFICATIONS,          16, CORE,       255, "NOTIF",           debugNotifications,      debugnotifications,        "notifications",  "enabled",    "All Notifications")                                                                           \
  /* Bits 17-23: spare (future core singles)                                  */                                                                                                                                                                       \
  /* Bits 24-31: Memory. Mirrors the Performance group's Stack/Heap/Timing    */                                                                                                                                                                       \
  /* split — isolates per-task heap noise from stack watermarks from          */                                                                                                                                                                       \
  /* buffer-sizing logs.                                                      */                                                                                                                                                                       \
  X(MEMORY,                 24, MEMORY,     255, "MEMORY",          debugMemory,             debugmemory,               "memory",         "enabled",    "All Memory") /* parent */                                                                     \
  X(MEMORY_HEAP,            25, MEMORY,      24, "MEMORY_HEAP",     debugMemoryHeap,         debugmemoryheap,           "memory",         "heap",       "Heap") /* [HEAP] per-task free/min/largest, [HEAP_MONITOR] DRAM low watermarks */             \
  X(MEMORY_STACK,           26, MEMORY,      24, "MEMORY_STACK",    debugMemoryStack,        debugmemorystack,          "memory",         "stack",      "Stack") /* [STACK] per-task watermark + peak reports */                                       \
  X(MEMORY_BUFFERS,         27, MEMORY,      24, "MEMORY_BUFFERS",  debugMemoryBuffers,      debugmemorybuffers,        "memory",         "buffers",    "Buffers") /* [JSON_RESP_BUF], [COOKIE_BUF] sizing diagnostics */                              \
  /* Bits 28-31: spare (Memory)                                               */                                                                                                                                                                       \
  /* Bits 32-47: ESP-NOW (double-width bank — the mesh roadmap will grow it). */                                                                                                                                                                       \
  /* No parent links: DEBUG_ESPNOWF gates on ESPNOW_CORE alone, STREAM and    */                                                                                                                                                                       \
  /* METADATA macros pass their bare bit, ROUTER/MESH/TOPO have no producer   */                                                                                                                                                                       \
  /* macros yet.                                                              */                                                                                                                                                                       \
  X(ESPNOW_CORE,            32, ESPNOW,     255, "ESP-NOW",         debugEspNowCore,         debugespnowcore,           "esp-now",        "core",       "Core") /* tag keeps the legacy hyphenated spelling */                                         \
  X(ESPNOW_ROUTER,          33, ESPNOW,     255, "ESPNOW_ROUTER",   debugEspNowRouter,       debugespnowrouter,         "esp-now",        "router",     "Router")                                                                                      \
  X(ESPNOW_MESH,            34, ESPNOW,     255, "ESPNOW_MESH",     debugEspNowMesh,         debugespnowmesh,           "esp-now",        "mesh",       "Mesh")                                                                                        \
  X(ESPNOW_TOPO,            35, ESPNOW,     255, "ESPNOW_TOPO",     debugEspNowTopo,         debugespnowtopo,           "esp-now",        "topology",   "Topology")                                                                                    \
  X(ESPNOW_STREAM,          36, ESPNOW,     255, "ESPNOW_STREAM",   debugEspNowStream,       debugespnowstream,         "esp-now",        "stream",     "Stream")                                                                                      \
  /* bit 37 free (was DEBUG_ESPNOW_ENCRYPTION — removed pre-1.0: no producer ever gated on it) */                                                                                                                                                      \
  X(ESPNOW_METADATA,        38, ESPNOW,     255, "ESPNOW_META",     debugEspNowMetadata,     debugespnowmetadata,       "esp-now",        "metadata",   "Metadata") /* metadata exchange (REQ/RESP/PUSH/store) */                                      \
  /* Bits 39-47: spare (ESP-NOW)                                              */                                                                                                                                                                       \
  /* Bits 48-55: MQTT                                                         */                                                                                                                                                                       \
  X(MQTT,                   48, MQTT,       255, "MQTT",            debugMqtt,               debugmqtt,                 "mqtt",           "enabled",    "All MQTT") /* parent */                                                                       \
  X(MQTT_CONNECTION,        49, MQTT,        48, "MQTT_CONN",       debugMqttConnection,     debugmqttconnection,       "mqtt",           "connection", "Connection") /* connect/disconnect, TLS config, broker errors, client init */                 \
  X(MQTT_PUBSUB,            50, MQTT,        48, "MQTT_PUBSUB",     debugMqttPubsub,         debugmqttpubsub,           "mqtt",           "pubsub",     "Pub/Sub") /* subscribe events, publish results, JSON buffer alloc, received messages */       \
  X(MQTT_DISCOVERY,         51, MQTT,        48, "MQTT_DISCOVERY",  debugMqttDiscovery,      debugmqttdiscovery,        "mqtt",           "discovery",  "Discovery") /* Home Assistant auto-discovery configs, base topic generation */                \
  X(MQTT_COMMANDS,          52, MQTT,        48, "MQTT_CMD",        debugMqttCommands,       debugmqttcommands,         "mqtt",           "commands",   "Commands") /* inbound MQTT command parsing, auth, response */                                 \
  /* Bits 53-55: spare (MQTT)                                                 */                                                                                                                                                                       \
  /* Bits 56-63: Automations. Subs are bare bits — no PARENT|SUB macros exist. */                                                                                                                                                                      \
  X(AUTOMATIONS,            56, AUTO,       255, "AUTO",            debugAutomations,        debugautomations,          "automations",    "enabled",    "All Automations") /* parent */                                                                \
  X(AUTO_EXEC,              57, AUTO,       255, "AUTO_EXEC",       debugAutoExec,           debugautoexec,             "automations",    "execution",  "Execution")                                                                                   \
  X(AUTO_CONDITION,         58, AUTO,       255, "AUTO_COND",       debugAutoCondition,      debugautocondition,        "automations",    "condition",  "Condition")                                                                                   \
  X(AUTO_TIMING,            59, AUTO,       255, "AUTO_TIME",       debugAutoTiming,         debugautotiming,           "automations",    "timing",     "Timing")                                                                                      \
  X(AUTO_SCHEDULER,         60, AUTO,       255, "AUTO_SCHED",      debugAutoScheduler,      debugautoscheduler,        "automations",    "scheduler",  "Scheduler")                                                                                   \
  /* Bits 61-63: spare (Automations)                                          */                                                                                                                                                                       \
  /* ---- Word 1 (bits 64-127): connectivity & features --------------------- */                                                                                                                                                                       \
  /* Bits 64-71: Bluetooth. Subs are bare bits: BLE_DEBUGF(flag) tests only   */                                                                                                                                                                       \
  /* the flag it is passed (Bluetooth.cpp), so the parent is NOT a master     */                                                                                                                                                                       \
  /* switch for the subs today.                                               */                                                                                                                                                                       \
  X(BLUETOOTH,              64, BT,         255, "BT",              debugBluetooth,          debugbluetooth,            "bluetooth",      "enabled",    "All Bluetooth")                                                                               \
  X(BLUETOOTH_CORE,         65, BT,         255, "BT_CORE",         debugBluetoothCore,      debugbluetoothcore,        "bluetooth",      "core",       "Core") /* BLE core lifecycle (init/connect/disconnect) */                                     \
  X(BLUETOOTH_GATT,         66, BT,         255, "BT_GATT",         debugBluetoothGatt,      debugbluetoothgatt,        "bluetooth",      "gatt",       "GATT") /* BLE GATT operations (read/write/notify) */                                          \
  X(BLUETOOTH_DATA,         67, BT,         255, "BT_DATA",         debugBluetoothData,      debugbluetoothdata,        "bluetooth",      "data",       "Data") /* BLE command/data transfer */                                                        \
  /* Bits 68-71: spare (Bluetooth)                                            */                                                                                                                                                                       \
  /* Bits 72-87: G2 smart glasses (double-width bank — fastest-growing        */                                                                                                                                                                       \
  /* family). All gated through DEBUG_G2_*F macros so the parent toggle       */                                                                                                                                                                       \
  /* still works as a master switch — sub-flags refine *which* G2 noise       */                                                                                                                                                                       \
  /* gets through.                                                            */                                                                                                                                                                       \
  X(G2,                     72, G2,         255, "G2",              debugG2,                 debugg2,                   "g2",             "enabled",    "All G2") /* parent (BLE link to glasses) */                                                   \
  X(G2_LIFECYCLE,           73, G2,          72, "G2_LIFE",         debugG2Lifecycle,        debugg2lifecycle,          "g2",             "lifecycle",  "Lifecycle") /* scan, BLE connect/disconnect, MTU, service enumeration */                      \
  X(G2_PROTOCOL,            74, G2,          72, "G2_PROTO",        debugG2Protocol,         debugg2protocol,           "g2",             "protocol",   "Protocol") /* envelope TX/RX, CRC, fragmentation, parse errors */                             \
  X(G2_EVENTS,              75, G2,          72, "G2_EVT",          debugG2Events,           debugg2events,             "g2",             "events",     "Events") /* DevEvents, ListEvents, SysEvents, gestures, taps */                               \
  X(G2_PAGES,               76, G2,          72, "G2_PAGE",         debugG2Pages,            debugg2pages,              "g2",             "pages",      "Pages") /* page-swap worker, hijack, CREATE-list/text, lens state */                          \
  X(G2_HEARTBEAT,           77, G2,          72, "G2_HB",           debugG2Heartbeat,        debugg2heartbeat,          "g2",             "heartbeat",  "Heartbeat") /* heartbeat TX + HeartbeatAck (every ~5s; loud) */                               \
  X(G2_DUMP,                78, G2,          72, "G2_DUMP",         debugG2Dump,             debugg2dump,               "g2",             "dump",       "Dump") /* [G2-DUMP] diagnostic ring buffer dumps on errors */                                 \
  /* Bits 79-87: spare (G2)                                                   */                                                                                                                                                                       \
  /* Bits 88-95: ESP-SR speech recognition. DEBUG_SR alone matches the legacy */                                                                                                                                                                       \
  /* gSrDebugLevel "level 1" (lifecycle + wake + commands); the sub-flags add */                                                                                                                                                                       \
  /* selective verbosity that previously required raising the global level    */                                                                                                                                                                       \
  /* (which dragged in unrelated noise). Only AFE and LIFECYCLE are enqueued  */                                                                                                                                                                       \
  /* as SR|sub (System_ESPSR.cpp); WAKE/COMMAND/TUNING have no gating macro   */                                                                                                                                                                       \
  /* today, so they carry no parent link.                                     */                                                                                                                                                                       \
  X(SR,                     88, SR,         255, "SR",              debugSr,                 debugsr,                   "espsr",          "enabled",    "All SR") /* parent: any SR debug */                                                           \
  X(SR_WAKE,                89, SR,         255, "SR_WAKE",         debugSrWake,             debugsrwake,               "espsr",          "wake",       "Wake word") /* wake word detection events */                                                  \
  X(SR_COMMAND,             90, SR,         255, "SR_CMD",          debugSrCommand,          debugsrcommand,            "espsr",          "command",    "Command match") /* command recognition + matching */                                          \
  X(SR_AFE,                 91, SR,          88, "SR_AFE",          debugSrAfe,              debugsrafe,                "espsr",          "afe",        "AFE / VAD") /* AFE/audio chain (VAD, noise, gain) */                                          \
  X(SR_LIFECYCLE,           92, SR,          88, "SR_LIFE",         debugSrLifecycle,        debugsrlifecycle,          "espsr",          "lifecycle",  "Lifecycle") /* init / start / stop verbose */                                                 \
  X(SR_TUNING,              93, SR,         255, "SR_TUNE",         debugSrTuning,           debugsrtuning,             "espsr",          "tuning",     "Tuning / threshold") /* auto-tune + confidence threshold */                                   \
  /* Bits 94-95: spare (SR)                                                   */                                                                                                                                                                       \
  /* Bits 96-103: On-device LLM (llama2.c / System_LLM). Subs are bare bits — */                                                                                                                                                                       \
  /* the DEBUG_LLM_*F macros pass the sub alone.                              */                                                                                                                                                                       \
  X(LLM,                    96, LLM,        255, "LLM",             debugLlm,                debugllm,                  "llm",            "enabled",    "All LLM") /* parent (all LLM debug) */                                                        \
  X(LLM_LOAD,               97, LLM,        255, "LLM_LOAD",        debugLlmLoad,            debugllmload,              "llm",            "load",       "Load / checkpoint") /* checkpoint load, header validation, weight mapping */                  \
  X(LLM_TOKENIZER,          98, LLM,        255, "LLM_TOK",         debugLlmTokenizer,       debugllmtokenizer,         "llm",            "tokenizer",  "Tokenizer") /* tokenizer file, BPE encode/decode */                                           \
  X(LLM_FORWARD,            99, LLM,        255, "LLM_FWD",         debugLlmForward,         debugllmforward,           "llm",            "forward",    "Forward") /* transformer forward (per-step; use sparingly) */                                 \
  X(LLM_GENERATE,          100, LLM,        255, "LLM_GEN",         debugLlmGenerate,        debugllmgenerate,          "llm",            "generate",   "Generate") /* generation loop, sampling, throughput */                                        \
  X(LLM_MEMORY,            101, LLM,        255, "LLM_MEM",         debugLlmMemory,          debugllmmemory,            "llm",            "memory",     "Memory / PSRAM") /* PSRAM estimates, context cap, allocations */                              \
  /* Bits 102-103: spare (LLM)                                                */                                                                                                                                                                       \
  /* Bits 104-111: Maps. Subs are bare bits — DEBUG_MAPS_*F pass the sub alone. */                                                                                                                                                                     \
  X(MAPS,                  104, MAPS,       255, "MAPS",            debugMaps,               debugmaps,                 "maps",           "enabled",    "All Maps") /* parent */                                                                       \
  X(MAPS_LOADING,          105, MAPS,       255, "MAPS_LOAD",       debugMapsLoading,        debugmapsloading,          "maps",           "loading",    "Loading") /* map file loading, tile directory parsing */                                      \
  X(MAPS_RENDERING,        106, MAPS,       255, "MAPS_RENDER",     debugMapsRendering,      debugmapsrendering,        "maps",           "rendering",  "Rendering") /* map render pipeline, feature drawing, viewport */                              \
  X(MAPS_PERF,             107, MAPS,       255, "MAPS_PERF",       debugMapsPerf,           debugmapsperf,             "maps",           "perf",       "Performance") /* performance timing (render ms, tile I/O, cache, FPS) */                      \
  /* Bits 108-111: spare (Maps)                                               */                                                                                                                                                                       \
  /* Bits 112-119: Camera. All gated through DEBUG_CAMERA_*F macros with      */                                                                                                                                                                       \
  /* `DEBUG_CAMERA | DEBUG_CAMERA_<sub>`, so the parent toggle still works    */                                                                                                                                                                       \
  /* as a master switch and sub-flags refine *which* camera noise gets        */                                                                                                                                                                       \
  /* through.                                                                 */                                                                                                                                                                       \
  X(CAMERA,                112, CAMERA,     255, "CAMERA",          debugCamera,             debugcamera,               "camera",         "enabled",    "All Camera") /* parent */                                                                     \
  X(CAMERA_LIFECYCLE,      113, CAMERA,     112, "CAMERA_LIFECYCLE", debugCameraLifecycle,    debugcameralifecycle,      "camera",         "lifecycle",  "Lifecycle") /* initCamera(), stopCamera(), PWDN/RESET sequencing, GPIO state */              \
  X(CAMERA_CAPTURE,        114, CAMERA,     112, "CAMERA_CAPTURE",  debugCameraCapture,      debugcameracapture,        "camera",         "capture",    "Capture") /* captureFrame(), JPEG validation, frame buffer, recovery path */                  \
  X(CAMERA_SETTINGS,       115, CAMERA,     112, "CAMERA_SETTINGS", debugCameraSettings,     debugcamerasettings,       "camera",         "settings",   "Settings") /* runtime resolution / quality / sensor register changes */                       \
  X(CAMERA_VIDEO,          116, CAMERA,     112, "CAMERA_VIDEO",    debugCameraVideo,        debugcameravideo,          "camera",         "video",      "Video") /* video recording start/finalize, frame writing, encoder state */                    \
  /* Bits 117-119: spare (Camera)                                             */                                                                                                                                                                       \
  /* Bits 120-127: I2C bus. Bus enable/disable + sensor auto-start happen at  */                                                                                                                                                                       \
  /* runtime, not just boot, so each is independently silenceable.            */                                                                                                                                                                       \
  X(I2C,                   120, I2C,        255, "I2C",             debugI2C,                debugi2c,                  "i2c",            "enabled",    "All I2C") /* parent: bus operations, transactions, clock changes, mutex */                    \
  X(I2C_BUS,               121, I2C,        120, "I2C_BUS",         debugI2CBus,             debugi2cbus,               "i2c",            "bus",        "Bus") /* [I2C] bus lifecycle, polling pause/resume, status bumps, raw transactions */         \
  X(I2C_DISCOVERY,         122, I2C,        120, "I2C_DISCOVERY",   debugI2CDiscovery,       debugi2cdiscovery,         "i2c",            "discovery",  "Discovery") /* [Discovery] / [I2C_REGISTRY] / [I2C_SENSORS] — probing, registration, scans */ \
  X(I2C_AUTOSTART,         123, I2C,        120, "I2C_AUTOSTART",   debugI2CAutoStart,       debugi2cautostart,         "i2c",            "autoStart",  "AutoStart") /* [AutoStart] sensor auto-start orchestration + per-sensor init results */       \
  /* Bits 124-127: spare (I2C)                                                */                                                                                                                                                                       \
  /* ---- Words 2-3 (bits 128-255): sensors — one byte each ----------------- */                                                                                                                                                                       \
  /* Uniform per-sensor layout: parent, then LIFECYCLE / POLLING / VALUES,    */                                                                                                                                                                       \
  /* then 4 spare bits. Macros gate on parent-OR-sub like every other family. */                                                                                                                                                                       \
  /*   LIFECYCLE — init, connect/disconnect, recovery, error retries          */                                                                                                                                                                       \
  /*   POLLING   — poll/sample cadence, capture timing, FPS, frame events     */                                                                                                                                                                       \
  /*   VALUES    — parsed readings, value-change events, data processing      */                                                                                                                                                                       \
  /* Bits 128-135: GPS (PA1010D)                                              */                                                                                                                                                                       \
  X(GPS,                   128, GPS,        255, "GPS",             debugGps,                debuggps,                  "gps",            "enabled",    "All GPS")                                                                                     \
  X(GPS_LIFECYCLE,         129, GPS,        128, "GPS_LIFE",        debugGpsLifecycle,       debuggpslifecycle,         "gps",            "lifecycle",  "Lifecycle")                                                                                   \
  X(GPS_POLLING,           130, GPS,        128, "GPS_POLL",        debugGpsPolling,         debuggpspolling,           "gps",            "polling",    "Polling")                                                                                     \
  X(GPS_VALUES,            131, GPS,        128, "GPS_VAL",         debugGpsValues,          debuggpsvalues,            "gps",            "values",     "Values")                                                                                      \
  /* Bits 136-143: RTC (DS3231)                                               */                                                                                                                                                                       \
  X(RTC,                   136, RTC,        255, "RTC",             debugRtc,                debugrtc,                  "rtc",            "enabled",    "All RTC")                                                                                     \
  X(RTC_LIFECYCLE,         137, RTC,        136, "RTC_LIFE",        debugRtcLifecycle,       debugrtclifecycle,         "rtc",            "lifecycle",  "Lifecycle")                                                                                   \
  X(RTC_POLLING,           138, RTC,        136, "RTC_POLL",        debugRtcPolling,         debugrtcpolling,           "rtc",            "polling",    "Polling")                                                                                     \
  X(RTC_VALUES,            139, RTC,        136, "RTC_VAL",         debugRtcValues,          debugrtcvalues,            "rtc",            "values",     "Values")                                                                                      \
  /* Bits 144-151: IMU (BNO055)                                               */                                                                                                                                                                       \
  X(IMU,                   144, IMU,        255, "IMU",             debugImu,                debugimu,                  "imu",            "enabled",    "All IMU")                                                                                     \
  X(IMU_LIFECYCLE,         145, IMU,        144, "IMU_LIFE",        debugImuLifecycle,       debugimulifecycle,         "imu",            "lifecycle",  "Lifecycle")                                                                                   \
  X(IMU_POLLING,           146, IMU,        144, "IMU_POLL",        debugImuPolling,         debugimupolling,           "imu",            "polling",    "Polling") /* frame timing, cache operations */                                                \
  X(IMU_VALUES,            147, IMU,        144, "IMU_VAL",         debugImuValues,          debugimuvalues,            "imu",            "values",     "Values") /* data updates */                                                                   \
  /* Bits 152-159: Thermal (MLX90640)                                         */                                                                                                                                                                       \
  X(THERMAL,               152, THERMAL,    255, "THERMAL",         debugThermal,            debugthermal,              "thermal",        "enabled",    "All Thermal")                                                                                 \
  X(THERMAL_LIFECYCLE,     153, THERMAL,    152, "THERMAL_LIFE",    debugThermalLifecycle,   debugthermallifecycle,     "thermal",        "lifecycle",  "Lifecycle")                                                                                   \
  X(THERMAL_POLLING,       154, THERMAL,    152, "THERMAL_POLL",    debugThermalPolling,     debugthermalpolling,       "thermal",        "polling",    "Polling") /* frame timing, capture, FPS */                                                    \
  X(THERMAL_VALUES,        155, THERMAL,    152, "THERMAL_VAL",     debugThermalValues,      debugthermalvalues,        "thermal",        "values",     "Values") /* interpolation, processing */                                                      \
  /* Bits 160-167: ToF (VL53L4CX)                                             */                                                                                                                                                                       \
  X(TOF,                   160, TOF,        255, "TOF",             debugTof,                debugtof,                  "tof",            "enabled",    "All ToF")                                                                                     \
  X(TOF_LIFECYCLE,         161, TOF,        160, "TOF_LIFE",        debugTofLifecycle,       debugtoflifecycle,         "tof",            "lifecycle",  "Lifecycle")                                                                                   \
  X(TOF_POLLING,           162, TOF,        160, "TOF_POLL",        debugTofPolling,         debugtofpolling,           "tof",            "polling",    "Polling") /* frame capture, object detection */                                               \
  X(TOF_VALUES,            163, TOF,        160, "TOF_VAL",         debugTofValues,          debugtofvalues,            "tof",            "values",     "Values")                                                                                      \
  /* Bits 168-175: Gamepad (Seesaw) — the shared input-abstraction layer      */                                                                                                                                                                       \
  X(INPUT,                 168, INPUT,      255, "INPUT",           debugInput,              debuginput,                "input",          "enabled",    "All Input")                                                                                   \
  X(INPUT_LIFECYCLE,       169, INPUT,      168, "INPUT_LIFE",      debugInputLifecycle,     debuginputlifecycle,       "input",          "lifecycle",  "Lifecycle")                                                                                   \
  X(INPUT_POLLING,         170, INPUT,      168, "INPUT_POLL",      debugInputPolling,       debuginputpolling,         "input",          "polling",    "Polling") /* frame timing, connection */                                                      \
  X(INPUT_VALUES,          171, INPUT,      168, "INPUT_VAL",       debugInputValues,        debuginputvalues,          "input",          "values",     "Values") /* button press/release events */                                                    \
  /* Bits 176-183: APDS (APDS9960)                                            */                                                                                                                                                                       \
  X(APDS,                  176, APDS,       255, "APDS",            debugApds,               debugapds,                 "apds",           "enabled",    "All APDS")                                                                                    \
  X(APDS_LIFECYCLE,        177, APDS,       176, "APDS_LIFE",       debugApdsLifecycle,      debugapdslifecycle,        "apds",           "lifecycle",  "Lifecycle")                                                                                   \
  X(APDS_POLLING,          178, APDS,       176, "APDS_POLL",       debugApdsPolling,        debugapdspolling,          "apds",           "polling",    "Polling") /* frame timing, connection */                                                      \
  X(APDS_VALUES,           179, APDS,       176, "APDS_VAL",        debugApdsValues,         debugapdsvalues,           "apds",           "values",     "Values")                                                                                      \
  /* Bits 184-191: Presence (STHS34PF80)                                      */                                                                                                                                                                       \
  X(PRESENCE,              184, PRESENCE,   255, "PRESENCE",        debugPresence,           debugpresence,             "presence",       "enabled",    "All Presence")                                                                                \
  X(PRESENCE_LIFECYCLE,    185, PRESENCE,   184, "PRESENCE_LIFE",   debugPresenceLifecycle,  debugpresencelifecycle,    "presence",       "lifecycle",  "Lifecycle")                                                                                   \
  X(PRESENCE_POLLING,      186, PRESENCE,   184, "PRESENCE_POLL",   debugPresencePolling,    debugpresencepolling,      "presence",       "polling",    "Polling")                                                                                     \
  X(PRESENCE_VALUES,       187, PRESENCE,   184, "PRESENCE_VAL",    debugPresenceValues,     debugpresencevalues,       "presence",       "values",     "Values")                                                                                      \
  /* Bits 192-199: FM Radio (RDA5807)                                         */                                                                                                                                                                       \
  X(FMRADIO,               192, FMRADIO,    255, "FMRADIO",         debugFmRadio,            debugfmradio,              "fmradio",        "enabled",    "All FM Radio") /* parent: operations + I2C debugging */                                       \
  X(FMRADIO_LIFECYCLE,     193, FMRADIO,    192, "FMRADIO_LIFE",    debugFmRadioLifecycle,   debugfmradiolifecycle,     "fmradio",        "lifecycle",  "Lifecycle")                                                                                   \
  X(FMRADIO_POLLING,       194, FMRADIO,    192, "FMRADIO_POLL",    debugFmRadioPolling,     debugfmradiopolling,       "fmradio",        "polling",    "Polling")                                                                                     \
  X(FMRADIO_VALUES,        195, FMRADIO,    192, "FMRADIO_VAL",     debugFmRadioValues,      debugfmradiovalues,        "fmradio",        "values",     "Values")                                                                                      \
  /* Bits 200-207: Microphone                                                 */                                                                                                                                                                       \
  X(MICROPHONE,            200, MICROPHONE, 255, "MIC",             debugMicrophone,         debugmicrophone,           "microphone",     "enabled",    "All Microphone") /* parent */                                                                 \
  X(MIC_LIFECYCLE,         201, MICROPHONE, 200, "MIC_LIFE",        debugMicLifecycle,       debugmiclifecycle,         "microphone",     "lifecycle",  "Lifecycle")                                                                                   \
  X(MIC_POLLING,           202, MICROPHONE, 200, "MIC_POLL",        debugMicPolling,         debugmicpolling,           "microphone",     "polling",    "Polling")                                                                                     \
  X(MIC_VALUES,            203, MICROPHONE, 200, "MIC_VAL",         debugMicValues,          debugmicvalues,            "microphone",     "values",     "Values")                                                                                      \
  /* Bits 208-215: ANO rotary encoder. Separate from DEBUG_INPUT* (which      */                                                                                                                                                                       \
  /* gates the shared input-abstraction layer) and from the seesaw gamepad's  */                                                                                                                                                                       \
  /* flags — these only affect the ANO driver's internal logs (init,          */                                                                                                                                                                       \
  /* polling, chord state machine, axis toggle).                              */                                                                                                                                                                       \
  X(ANO_ENCODER,           208, ANO_ENCODER, 255, "ANO",             debugAnoEncoder,         debuganoencoder,           "anoencoder",     "enabled",    "All ANO Encoder")                                                                            \
  X(ANO_ENCODER_LIFECYCLE, 209, ANO_ENCODER, 208, "ANO_LIFE",        debugAnoEncoderLifecycle, debuganoencoderlifecycle,  "anoencoder",     "lifecycle",  "Lifecycle")                                                                                 \
  X(ANO_ENCODER_POLLING,   210, ANO_ENCODER, 208, "ANO_POLL",        debugAnoEncoderPolling,  debuganoencoderpolling,    "anoencoder",     "polling",    "Polling")                                                                                    \
  X(ANO_ENCODER_VALUES,    211, ANO_ENCODER, 208, "ANO_VAL",         debugAnoEncoderValues,   debuganoencodervalues,     "anoencoder",     "values",     "Values")                                                                                     \
  /* Bits 216-223: authenticated CM5 UART host link. The parent is an        */                                                                                                                                                                       \
  /* explicit master; sub-macros gate on parent|sub like the sensor banks.   */                                                                                                                                                                       \
  X(UART,                  216, UART,        255, "UART",             debugUart,               debuguart,                "uart",           "enabled",    "All UART")                                                                                   \
  X(UART_LIFECYCLE,        217, UART,        216, "UART_LIFE",        debugUartLifecycle,      debuguartlifecycle,       "uart",           "lifecycle",  "Lifecycle")                                                                                  \
  X(UART_CONTROL,          218, UART,        216, "UART_CTRL",        debugUartControl,        debuguartcontrol,         "uart",           "control",    "Control plane") /* heartbeat, lease renewal, CM5 ACK/report and EVT summaries */            \
  /* Bits 219-223: spare (UART)                                               */                                                                                                                                                                       \
  /* Bits 224-239: Even Realities R1 ring (BLE health ring). The parent is an */                                                                                                                                                                       \
  /* explicit master; sub-macros gate on parent|sub like the G2/sensor banks. */                                                                                                                                                                       \
  X(RING,                  224, RING,        255, "RING",             debugRing,               debugring,                "ring",           "enabled",    "All R1 Ring")                                                                                \
  X(RING_LIFECYCLE,        225, RING,        224, "RING_LIFE",        debugRingLifecycle,      debugringlifecycle,       "ring",           "lifecycle",  "Lifecycle") /* scan, connect admission, GATT discovery, notify subscribe, disconnect */     \
  X(RING_SETUP,            226, RING,        224, "RING_SETUP",       debugRingSetup,          debugringsetup,           "ring",           "setup",      "Setup ritual") /* auth/deviceInfo/advStart stages, clock custody, protocol self-test */    \
  X(RING_PROTOCOL,         227, RING,        224, "RING_PROTO",       debugRingProtocol,       debugringprotocol,        "ring",           "protocol",   "Protocol") /* per-frame envelope decode, rejects, dup serial, reassembly */               \
  X(RING_TXN,              228, RING,        224, "RING_TXN",         debugRingTxn,            debugringtxn,             "ring",           "txn",        "Transactions") /* intent queued + TX writes + packetAck */                                \
  X(RING_HEALTH,           229, RING,        224, "RING_HEALTH",      debugRingHealth,         debugringhealth,          "ring",           "health",     "Health data") /* telemetry cache, history sweep coordinator, model ingest */             \
  X(RING_BRIDGE,           230, RING,        224, "RING_BRIDGE",      debugRingBridge,         debugringbridge,          "ring",           "bridge",     "Spoof bridge") /* sid=0x90 RingDataPackage push, spoof task, bridge heartbeat */          \
  X(RING_DUMP,             231, RING,        224, "RING_DUMP",        debugRingDump,           debugringdump,            "ring",           "dump",       "Hex dumps") /* raw notify/payload/fragment hex; redaction still applies */               \
  /* Bits 232-239: spare (RING)                                               */                                                                                                                                                                       \
  /* Bits 240-247: spare (one whole bank for a future sensor/subsystem)       */                                                                                                                                                                       \
  /* Bits 248-255: Control. Not subsystems — never name one of these as a     */                                                                                                                                                                       \
  /* message's only flag. Control rows carry an empty TAG (deliberately       */                                                                                                                                                                       \
  /* absent from the category-name walk): the tag must resolve to the real    */                                                                                                                                                                       \
  /* producer riding alongside.                                               */                                                                                                                                                                       \
  X(ALWAYS,                255, CONTROL,    255, "",                DBG_NO_SETTING,          DBG_NO_CMD,                "",               "",           "") /* emit regardless of flag state; OR with the producer's own flag */                       \
  /* Bits 248-254: spare (Control) */

// Many-to-one extras: settings fields that raise a flag ANOTHER field already
// carries. The flag row above keeps the PRIMARY field; each extra becomes one
// more settings→flag map row in System_Settings.cpp. (debugAuthCookies is
// also a bitless runtime sub below — the map row is what makes the persisted
// bool rebuild DEBUG_AUTH at boot.)
//   X(field, SYM)
#define DBG_FLAG_EXTRA_SETTINGS(X) \
  X(debugAuthCookies, AUTH)

// --- Generated: dense index ------------------------------------------------
enum DbgFlagIdx : uint8_t {
#define DBG_X(SYM, bit, BANK, parentBit, tag, settingsField, ...) DBG_##SYM,
  DBG_FLAG_LIST(DBG_X)
#undef DBG_X
  DBG_FLAG_COUNT
};

// --- Generated: the DEBUG_* mask constants. Same spellings as the old
// #defines, so every existing call site compiles unchanged. -----------------
#define DBG_X(SYM, bit, BANK, parentBit, tag, settingsField, ...) \
  inline constexpr DebugFlagMask DEBUG_##SYM = DEBUG_BIT(bit);
DBG_FLAG_LIST(DBG_X)
#undef DBG_X

// --- Generated: parallel columns, indexed by DbgFlagIdx --------------------
#define DBG_X(SYM, bit, BANK, parentBit, tag, settingsField, ...) bit,
inline constexpr uint8_t kDbgBit[DBG_FLAG_COUNT] = { DBG_FLAG_LIST(DBG_X) };
#undef DBG_X

#define DBG_X(SYM, bit, BANK, parentBit, tag, settingsField, ...) parentBit,
inline constexpr uint8_t kDbgParentBit[DBG_FLAG_COUNT] = { DBG_FLAG_LIST(DBG_X) };  // 255 = no parent (root row)
#undef DBG_X

#define DBG_X(SYM, bit, BANK, parentBit, tag, settingsField, ...) DBG_BANK_##BANK,
inline constexpr DbgBank kDbgBank[DBG_FLAG_COUNT] = { DBG_FLAG_LIST(DBG_X) };
#undef DBG_X

#define DBG_X(SYM, bit, BANK, parentBit, tag, settingsField, ...) tag,
inline constexpr const char* kDbgTag[DBG_FLAG_COUNT] = { DBG_FLAG_LIST(DBG_X) };    // "" = control bit, never a category
#undef DBG_X

#define DBG_X(SYM, bit, BANK, parentBit, tag, settingsField, ...) DEBUG_BIT(bit),
inline constexpr DebugFlagMask kDbgMask[DBG_FLAG_COUNT] = { DBG_FLAG_LIST(DBG_X) };
#undef DBG_X

// ============================================================================
// Bitless subs — the 40 settings with NO bit of their own. Their runtime
// layer is a DebugSubFlags bool (toggled alone by `temp`), their persistent
// layer the gSettings bool; the only mask effect they have is OR-ing up into
// their family's parent bit via dbgRecomputeParent(). PARENT_SYM must name a
// root DBG_FLAG_LIST row that DBG_AGG_FAMILY_LIST marks SUBBOOLS — both are
// static_assert-enforced below.
// ============================================================================
//   S(SYM, subField, settingsField, PARENT_SYM, cmdIdent, "group", "jsonKey", "label")
//     subField      — DebugSubFlags member (runtime layer)
//     settingsField — gSettings member (persistent layer)
//     cmdIdent/group/jsonKey/label — settings-registry columns, exactly as in
//                     DBG_FLAG_LIST above: verbatim transcriptions, never
//                     computed; registry order lives in the pick list in
//                     System_Settings.cpp
#define DBG_SUBBOOL_LIST(S) \
  S(AUTH_SESSIONS,       authSessions,       debugAuthSessions,       AUTH,        debugauthsessions,        "authentication", "sessions",    "Sessions")            \
  S(AUTH_COOKIES,        authCookies,        debugAuthCookies,        AUTH,        debugauthcookies,         "authentication", "cookies",     "Cookies")             \
  S(AUTH_LOGIN,          authLogin,          debugAuthLogin,          AUTH,        debugauthlogin,           "authentication", "login",       "Login")               \
  S(AUTH_BOOTID,         authBootId,         debugAuthBootId,         AUTH,        debugauthbootid,          "authentication", "bootId",      "Boot ID")             \
  S(HTTP_HANDLERS,       httpHandlers,       debugHttpHandlers,       HTTP,        debughttphandlers,        "http",          "handlers",    "Handlers")             \
  S(HTTP_REQUESTS,       httpRequests,       debugHttpRequests,       HTTP,        debughttprequests,        "http",          "requests",    "Requests")             \
  S(HTTP_RESPONSES,      httpResponses,      debugHttpResponses,      HTTP,        debughttpresponses,       "http",          "responses",   "Responses")            \
  S(HTTP_STREAMING,      httpStreaming,      debugHttpStreaming,      HTTP,        debughttpstreaming,       "http",          "streaming",   "Streaming")            \
  S(WIFI_CONNECTION,     wifiConnection,     debugWifiConnection,     WIFI,        debugwificonnection,      "wifi",          "connection",  "Connection")           \
  S(WIFI_CONFIG,         wifiConfig,         debugWifiConfig,         WIFI,        debugwificonfig,          "wifi",          "config",      "Config")               \
  S(WIFI_SCANNING,       wifiScanning,       debugWifiScanning,       WIFI,        debugwifiscanning,        "wifi",          "scanning",    "Scanning")             \
  S(WIFI_DRIVER,         wifiDriver,         debugWifiDriver,         WIFI,        debugwifidriver,          "wifi",          "driver",      "Driver")               \
  S(STORAGE_FILES,       storageFiles,       debugStorageFiles,       STORAGE,     debugstoragefiles,        "storage",       "files",       "Files")                \
  S(STORAGE_JSON,        storageJson,        debugStorageJson,        STORAGE,     debugstoragejson,         "storage",       "json",        "JSON")                 \
  S(STORAGE_SETTINGS,    storageSettings,    debugStorageSettings,    STORAGE,     debugstoragesettings,     "storage",       "settings",    "Settings")             \
  S(STORAGE_MIGRATION,   storageMigration,   debugStorageMigration,   STORAGE,     debugstoragemigration,    "storage",       "migration",   "Migration")            \
  S(STORAGE_PERMISSIONS, storagePermissions, debugStoragePermissions, STORAGE,     debugstoragepermissions,  "storage",       "permissions", "Permissions")          \
  S(SYSTEM_BOOT,         systemBoot,         debugSystemBoot,         SYSTEM,      debugsystemboot,          "system",        "boot",        "Boot")                 \
  S(SYSTEM_CONFIG,       systemConfig,       debugSystemConfig,       SYSTEM,      debugsystemconfig,        "system",        "config",      "Config")               \
  S(SYSTEM_TASKS,        systemTasks,        debugSystemTasks,        SYSTEM,      debugsystemtasks,         "system",        "tasks",       "Tasks")                \
  S(SYSTEM_HARDWARE,     systemHardware,     debugSystemHardware,     SYSTEM,      debugsystemhardware,      "system",        "hardware",    "Hardware")             \
  S(USERS_MGMT,          usersMgmt,          debugUsersMgmt,          USERS,       debugusersmgmt,           "users",         "management",  "Management")           \
  S(USERS_REGISTER,      usersRegister,      debugUsersRegister,      USERS,       debugusersregister,       "users",         "registration", "Registration")        \
  S(USERS_QUERY,         usersQuery,         debugUsersQuery,         USERS,       debugusersquery,          "users",         "query",       "Query")                \
  S(CLI_EXECUTION,       cliExecution,       debugCliExecution,       CLI,         debugcliexecution,        "cli",           "execution",   "Execution")            \
  S(CLI_QUEUE,           cliQueue,           debugCliQueue,           CLI,         debugcliqueue,            "cli",           "queue",       "Queue")                \
  S(CLI_VALIDATION,      cliValidation,      debugCliValidation,      CLI,         debugclivalidation,       "cli",           "validation",  "Validation")           \
  S(PERF_STACK,          perfStack,          debugPerfStack,          PERFORMANCE, debugperfstack,           "performance",   "stack",       "Stack")                \
  S(PERF_HEAP,           perfHeap,           debugPerfHeap,           PERFORMANCE, debugperfheap,            "performance",   "heap",        "Heap")                 \
  S(PERF_TIMING,         perfTiming,         debugPerfTiming,         PERFORMANCE, debugperftiming,          "performance",   "timing",      "Timing")               \
  S(SSE_CONNECTION,      sseConnection,      debugSseConnection,      SSE,         debugsseconnection,       "sse",           "connection",  "Connection")           \
  S(SSE_EVENTS,          sseEvents,          debugSseEvents,          SSE,         debugsseevents,           "sse",           "events",      "Events")               \
  S(SSE_BROADCAST,       sseBroadcast,       debugSseBroadcast,       SSE,         debugssebroadcast,        "sse",           "broadcast",   "Broadcast")            \
  S(CMDFLOW_ROUTING,     cmdflowRouting,     debugCmdflowRouting,     CMD_FLOW,    debugcmdflowrouting,      "commands",      "routing",     "Routing")              \
  S(CMDFLOW_QUEUE,       cmdflowQueue,       debugCmdflowQueue,       CMD_FLOW,    debugcmdflowqueue,        "commands",      "queue",       "Queue")                \
  S(CMDFLOW_CONTEXT,     cmdflowContext,     debugCmdflowContext,     CMD_FLOW,    debugcmdflowcontext,      "commands",      "context",     "Context")              \
  S(NTP_SYNC,            ntpSync,            debugDatetimeSync,       NTP,         debugdatetimesync,        "datetime",      "sync",        "Sync loop")            \
  S(NTP_SETUP,           ntpSetup,           debugDatetimeSetup,      NTP,         debugdatetimesetup,       "datetime",      "setup",       "Setup/configTime")     \
  S(NTP_ANCHOR,          ntpAnchor,          debugDatetimeAnchor,     NTP,         debugdatetimeanchor,      "datetime",      "anchor",      "Boot anchors")         \
  S(NTP_RESOLVE,         ntpResolve,         debugDatetimeResolve,    NTP,         debugdatetimeresolve,     "datetime",      "resolve",     "Timestamp resolution")

// --- Generated: dense sub index + parent column ----------------------------
enum DbgSubIdx : uint8_t {
#define DBG_S(SYM, subField, settingsField, PARENT_SYM, ...) DBG_SUB_##SYM,
  DBG_SUBBOOL_LIST(DBG_S)
#undef DBG_S
  DBG_SUBBOOL_COUNT
};

#define DBG_S(SYM, subField, settingsField, PARENT_SYM, ...) DBG_##PARENT_SYM,
inline constexpr DbgFlagIdx kDbgSubParentIdx[DBG_SUBBOOL_COUNT] = { DBG_SUBBOOL_LIST(DBG_S) };
#undef DBG_S

// ============================================================================
// Aggregated families — the ONLY parent bits anything recomputes. Every
// family absent from this list is an explicit master switch (G2, MQTT,
// CAMERA, MEMORY, MAPS, ESP-NOW, AUTO_*, I2C, HTTPS and all sensors): user
// toggles own their bit outright and NOTHING may rederive it. Three term
// shapes, verbatim from the pre-C1 sync helpers:
//   SUBBOOLS      — parent's own setting OR the family's bitless runtime subs
//   SETTINGS      — OR of every family row's persisted bool; NO runtime terms
//                   (LLM: temp sub toggles deliberately never aggregate)
//   SETTINGS_BITS — SETTINGS, plus any non-parent family-bank bit in the live
//                   mask (BT/SR: temp-set child bits must raise the parent
//                   because their output gates test parent alongside sub)
// ============================================================================
//   A(SYM, MODE)
#define DBG_AGG_FAMILY_LIST(A) \
  A(AUTH,        SUBBOOLS)      \
  A(HTTP,        SUBBOOLS)      \
  A(WIFI,        SUBBOOLS)      \
  A(STORAGE,     SUBBOOLS)      \
  A(SYSTEM,      SUBBOOLS)      \
  A(USERS,       SUBBOOLS)      \
  A(NTP,         SUBBOOLS)      \
  A(CLI,         SUBBOOLS)      \
  A(PERFORMANCE, SUBBOOLS)      \
  A(SSE,         SUBBOOLS)      \
  A(CMD_FLOW,    SUBBOOLS)      \
  A(LLM,         SETTINGS)      \
  A(BLUETOOTH,   SETTINGS_BITS) \
  A(SR,          SETTINGS_BITS)

enum DbgAggMode : uint8_t {
  DBG_AGG_NONE = 0,       // not aggregated — dbgRecomputeParent() must be a no-op
  DBG_AGG_SUBBOOLS,
  DBG_AGG_SETTINGS,
  DBG_AGG_SETTINGS_BITS,
};

// Dense per-flag mode column (zero-init = DBG_AGG_NONE for the other 103
// rows). Struct-wrapped C array: no <array> dependency in this fragment.
struct DbgAggModeTable { DbgAggMode m[DBG_FLAG_COUNT]; };
inline constexpr DbgAggModeTable kDbgAggMode = [] {
  DbgAggModeTable t{};
#define DBG_A(SYM, MODE) t.m[DBG_##SYM] = DBG_AGG_##MODE;
  DBG_AGG_FAMILY_LIST(DBG_A)
#undef DBG_A
  return t;
}();

// --- Compile-time invariants — a bad row is a build error, not a runtime
// mystery. O(n^2) over 120 rows is well under the constexpr step limit. -----

constexpr bool dbgStrEq(const char* a, const char* b) {
  while (*a != '\0' && *a == *b) { ++a; ++b; }
  return *a == *b;
}

// No two rows share a bit.
constexpr bool dbgBitsUnique() {
  for (int i = 0; i < DBG_FLAG_COUNT; ++i)
    for (int j = i + 1; j < DBG_FLAG_COUNT; ++j)
      if (kDbgBit[i] == kDbgBit[j]) return false;
  return true;
}

// Every row's bit lies inside [base, base+width) of its declared bank.
constexpr bool dbgBitsInDeclaredBank() {
  for (int i = 0; i < DBG_FLAG_COUNT; ++i) {
    const int lo = kDbgBankBase[kDbgBank[i]];
    const int hi = lo + kDbgBankWidth[kDbgBank[i]];
    if (kDbgBit[i] < lo || kDbgBit[i] >= hi) return false;
  }
  return true;
}

// Bank bit-ranges never overlap (gaps between banks are fine — they are the
// spare unnamed banks).
constexpr bool dbgBanksDisjoint() {
  for (int i = 0; i < DBG_BANK_COUNT; ++i)
    for (int j = i + 1; j < DBG_BANK_COUNT; ++j) {
      const int aLo = kDbgBankBase[i], aHi = aLo + kDbgBankWidth[i];
      const int bLo = kDbgBankBase[j], bHi = bLo + kDbgBankWidth[j];
      if (aLo < bHi && bLo < aHi) return false;
    }
  return true;
}

// Every tag is non-empty and unique — and CONTROL-bank rows must ALL stay
// tagless, or a tagged control row would win the category walk over the
// producer flag riding along in the same mask.
constexpr bool dbgTagsUniqueNonEmpty() {
  for (int i = 0; i < DBG_FLAG_COUNT; ++i) {
    if (kDbgBank[i] == DBG_BANK_CONTROL) {
      if (kDbgTag[i][0] != '\0') return false;
      continue;
    }
    if (kDbgTag[i][0] == '\0') return false;
    for (int j = i + 1; j < DBG_FLAG_COUNT; ++j)
      if (dbgStrEq(kDbgTag[i], kDbgTag[j])) return false;
  }
  return true;
}

// Every parentBit below 255 names the bit of an existing row; that row is
// itself a root (parentBit 255) and lives in the same bank. Bit 255 is the
// one control row: tagless, and never claimable as a parent (255 = "none").
constexpr bool dbgParentsWellFormed() {
  int controlRows = 0;
  for (int i = 0; i < DBG_FLAG_COUNT; ++i) {
    if (kDbgBit[i] == 255) {
      ++controlRows;
      if (kDbgTag[i][0] != '\0') return false;
    }
    if (kDbgParentBit[i] == 255) continue;  // root row — nothing to check
    bool found = false;
    for (int j = 0; j < DBG_FLAG_COUNT; ++j) {
      if (kDbgBit[j] != kDbgParentBit[i]) continue;
      found = true;
      if (kDbgParentBit[j] != 255) return false;   // parent must be a root
      if (kDbgBank[j] != kDbgBank[i]) return false; // parent shares the bank
      break;
    }
    if (!found) return false;
  }
  return controlRows == 1;
}

// Every bitless sub hangs off a root row that the agg table marks SUBBOOLS —
// a sub under a non-aggregated (or non-root) parent would silently never
// light its family. (PARENT_SYM naming a nonexistent row is already a
// compile error via DBG_##PARENT_SYM.)
constexpr bool dbgSubParentsWellFormed() {
  for (int i = 0; i < DBG_SUBBOOL_COUNT; ++i) {
    if (kDbgParentBit[kDbgSubParentIdx[i]] != 255) return false;
    if (kDbgAggMode.m[kDbgSubParentIdx[i]] != DBG_AGG_SUBBOOLS) return false;
  }
  return true;
}

// Every aggregated family names a root row, and the list carries exactly the
// 14 pre-C1 sync-helper families — adding one is a deliberate act, not drift.
constexpr bool dbgAggFamiliesWellFormed() {
  int n = 0;
  for (int i = 0; i < DBG_FLAG_COUNT; ++i) {
    if (kDbgAggMode.m[i] == DBG_AGG_NONE) continue;
    ++n;
    if (kDbgParentBit[i] != 255) return false;
  }
  return n == 14;
}

static_assert(DBG_FLAG_COUNT == 128, "debug-flag row count changed — re-check every kDbg* consumer and the settings/CLI/web layers before accepting");
static_assert(DBG_SUBBOOL_COUNT == 40, "bitless-sub row count changed — re-check DebugSubFlags, the CLI sub handlers, and dbgRecomputeParent consumers");
static_assert(dbgBitsUnique(),          "two debug-flag rows claim the same bit");
static_assert(dbgBitsInDeclaredBank(),  "a debug-flag bit falls outside its declared bank");
static_assert(dbgBanksDisjoint(),       "two banks overlap in DBG_BANK_LIST");
static_assert(dbgTagsUniqueNonEmpty(),  "tag invariant: duplicate tag, empty non-control tag, or tagged CONTROL row");
static_assert(dbgParentsWellFormed(),   "bad parent link: parentBit must name a root row in the same bank, and bit 255 must be the single tagless control row");
static_assert(dbgSubParentsWellFormed(),   "bitless sub's PARENT_SYM must be a root flag row marked SUBBOOLS in DBG_AGG_FAMILY_LIST");
static_assert(dbgAggFamiliesWellFormed(),  "DBG_AGG_FAMILY_LIST must hold exactly the 14 aggregated families, each a root row");
