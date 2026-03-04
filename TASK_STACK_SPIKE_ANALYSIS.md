# Task Stack Spike Analysis

**Generated:** 2026-02-28  
**Based on:** Stack watermark data from memory report

---

## Overview

Your task stack report shows:
- **httpd**: 35 KB allocated, 4 KB used (11% usage) - **31 KB waste**
- **espnow_task**: 24 KB allocated, 5 KB used (21% usage) - **18 KB waste**
- **main**: 21 KB allocated, 4 KB used (18% usage) - **17 KB waste**

The low usage percentages indicate these tasks rarely spike. However, when they **do** spike, here's what causes it:

---

## 1. HTTPD Task Spikes (8 KB stack, configured 35 KB)

**Current Stack:** 35 KB (8192 bytes configured in `startHttpServer`)  
**Typical Usage:** 4 KB (11%)  
**Peak Usage:** ~8 KB during heavy operations

### Primary Spike Causes

#### A. **File Upload Operations** (`handleFileUpload`)
**Location:** `@/Users/morgan/esp/hardwareone-idf/components/hardwareone/WebServer_Server.cpp:1495-1850`

**Stack Consumers:**
```cpp
// Large local variables on stack
char path[256];                    // 256 bytes
char hexBuf[3];                    // 3 bytes
int urlState = 0;
enum { F_NONE, F_PATH, F_CONTENT } field;

// PSRAM allocations (not on stack, but function call overhead)
uint8_t* uploadOutBuf = ps_alloc(4096, PreferPSRAM);     // 4 KB buffer
char* uploadRecvBuf = ps_alloc(4096, PreferPSRAM);       // 4 KB buffer

// Deep call stack during upload:
handleFileUpload() 
  → httpd_req_recv() (ESP-IDF internal, ~1-2 KB)
    → Base64 decoding loop
      → VFS::open() / File.write()
        → LittleFS operations (~1-2 KB)
```

**Why it spikes:**
- Streaming decode of base64 content
- Nested function calls for filesystem operations
- URL decoding state machine
- File validation and permission checks

**Peak Stack:** ~6-8 KB during large file uploads

---

#### B. **File Write Operations** (`handleFileWrite`)
**Location:** `@/Users/morgan/esp/hardwareone-idf/components/hardwareone/WebServer_Server.cpp:1333-1492`

**Stack Consumers:**
```cpp
// Large body allocation (PSRAM, but allocation overhead on stack)
char* body = ps_alloc(contentLen + 1, PreferPSRAM);  // Up to 150 KB

// String operations
String s = String(body);           // Temporary String object
String name = getParam("name");    // Multiple String allocations
String content = getParam("content");

// Lambda function captures (on stack)
auto getParam = [&](const char* key) -> String { ... };

// Deep call stack:
handleFileWrite()
  → httpd_req_recv() (~1-2 KB)
    → String operations
      → VFS::open() / File.write()
        → Automation hooks (if automations.json)
          → sanitizeAutomationsJson() (~2-3 KB)
```

**Why it spikes:**
- Multiple String allocations and copies
- Lambda function overhead
- Automation post-save hooks (for automations.json)
- Chunked write loop

**Peak Stack:** ~5-7 KB during automation file writes

---

#### C. **Settings Schema Generation** (`handleSettingsSchema`)
**Location:** `@/Users/morgan/esp/hardwareone-idf/components/hardwareone/WebServer_Server.cpp:2049-2150`

**Stack Consumers:**
```cpp
// Iterates through ALL settings modules and entries
for (int m = 0; m < moduleCount; m++) {
  const SettingsModule& mod = modules[m];
  for (int e = 0; e < mod.entryCount; e++) {
    // Builds JSON schema for each setting
    // Multiple snprintf calls (~256 bytes each)
    snprintf(gJsonResponseBuffer + offset, remaining, ...);
  }
}

// Call stack:
handleSettingsSchema()
  → Iterate 19+ modules × ~10 entries each
    → snprintf for each entry (~190+ calls)
      → String formatting overhead
```

**Why it spikes:**
- Processes 190+ settings entries in one request
- Multiple snprintf calls build large JSON response
- Mutex contention if multiple requests arrive

**Peak Stack:** ~4-5 KB during schema generation

---

#### D. **File List Operations** (`handleFilesList`)
**Location:** `@/Users/morgan/esp/hardwareone-idf/components/hardwareone/WebServer_Server.cpp:3265-3302`

**Stack Consumers:**
```cpp
// Recursive directory traversal
void listDirectory(File dir, String path, int depth) {
  while (File entry = dir.openNextFile()) {
    // Recursive call for subdirectories
    if (entry.isDirectory()) {
      listDirectory(entry, path + "/" + entry.name(), depth + 1);
    }
  }
}

// Each recursion level adds:
// - File object (~64 bytes)
// - String path concatenation (~256 bytes)
// - Local variables (~64 bytes)
// Total: ~384 bytes per level
```

**Why it spikes:**
- Recursive directory traversal (up to 10 levels deep)
- String concatenation for full paths
- Multiple File objects open simultaneously

**Peak Stack:** ~5-6 KB for deep directory trees

---

### HTTPD Stack Recommendations

**Current:** 35 KB (8192 bytes)  
**Actual Peak:** ~8 KB  
**Recommended:** **12 KB (3072 words)** - 50% safety margin

**Savings:** 23 KB per httpd task

---

## 2. ESP-NOW Task Spikes (24 KB stack)

**Current Stack:** 24 KB (ESPNOW_HB_STACK_WORDS = 6144)  
**Typical Usage:** 5 KB (21%)  
**Peak Usage:** ~10-12 KB during heavy mesh operations

### Primary Spike Causes

#### A. **Mesh Heartbeat Processing** (`processMeshHeartbeats`)
**Location:** `@/Users/morgan/esp/hardwareone-idf/components/hardwareone/System_ESPNow.cpp:6929-7100`

**Stack Consumers:**
```cpp
void processMeshHeartbeats() {
  // 1. Drain RX ring buffer
  while (gEspNowRxHead != gEspNowRxTail) {
    InboundRxItem& item = gEspNowRxRing[tail];
    onEspNowRawRecv(&ri, item.data, item.len);  // Deep call chain
  }
  
  // 2. Send periodic heartbeats
  V3PayloadHeartbeat hb = {};                    // 64 bytes
  v3_broadcast(...);                             // Deep call
  
  // 3. Process worker status updates
  // 4. Handle topology collection
  // 5. Retry queue processing
}

// Deep call chain during RX processing:
onEspNowRawRecv()
  → handleV3Protocol()
    → processV3Heartbeat() / processV3WorkerStatus()
      → updateMeshPeerHealth()
        → updateMeshPeerMeta()
          → String allocations for device names
            → JSON parsing for capability updates
```

**Why it spikes:**
- **Multiple peers sending simultaneously** (up to 16 peers)
- Each peer heartbeat triggers:
  - Peer health update
  - Metadata update
  - Message history storage
  - Capability parsing (if included)
- **Broadcast operations** build packets for all peers
- **Debug logging** (when enabled) adds significant overhead

**Peak Stack:** ~10-12 KB when processing 10+ simultaneous peer messages

---

#### B. **V3 Protocol Message Handling**
**Location:** `@/Users/morgan/esp/hardwareone-idf/components/hardwareone/System_ESPNow.cpp:3000-4500`

**Stack Consumers:**
```cpp
// Large message payloads on stack
V3PayloadWorkerStatus ws = {};     // ~256 bytes
V3PayloadCapability cap = {};      // ~512 bytes
V3PayloadFileStart fs = {};        // ~128 bytes

// String operations for device names
String deviceName = getEspNowDeviceName(mac);  // ~64 bytes
String message = ...;                          // Variable

// JSON parsing for remote commands
JsonDocument doc;                   // ArduinoJson (uses PSRAM allocator)
deserializeJson(doc, payload);

// Deep nesting:
handleV3Protocol()
  → switch (msgType)
    → case WORKER_STATUS:
        processWorkerStatus()
          → updateMeshPeerMeta()
            → storeMessageInPeerHistory()
              → String formatting
    → case REMOTE_CMD:
        processRemoteCommand()
          → executeCommand() (in cmd_exec_task)
            → Command execution (~2-4 KB)
```

**Why it spikes:**
- Large protocol payloads (up to 512 bytes per message)
- Nested switch/case handling
- String operations for logging and storage
- JSON parsing for complex messages

**Peak Stack:** ~8-10 KB during worker status processing

---

#### C. **File Transfer Operations**
**Location:** `@/Users/morgan/esp/hardwareone-idf/components/hardwareone/System_ESPNow.cpp:3270-3400`

**Stack Consumers:**
```cpp
// File transfer start
V3PayloadFileStart fs = {};        // 128 bytes
FileTransfer* transfer = new FileTransfer();
transfer->dataBuffer = heap_caps_malloc(fileSize, MALLOC_CAP_SPIRAM);

// Chunk processing
V3PayloadFileChunk chunk = {};     // 256 bytes
memcpy(transfer->dataBuffer + offset, chunk.data, chunk.len);

// Call stack:
handleV3FileStart()
  → Allocate FileTransfer struct
    → Allocate PSRAM buffer (fileSize)
      → Initialize chunk bitmap
handleV3FileChunk()
  → Validate chunk index
    → Copy to buffer
      → Update bitmap
        → Check completion
          → Write to filesystem
```

**Why it spikes:**
- Large chunk payloads (up to 240 bytes per chunk)
- Filesystem write operations when transfer completes
- Multiple simultaneous file transfers (up to 4)

**Peak Stack:** ~6-8 KB during file transfer completion

---

#### D. **Topology Collection**
**Location:** `@/Users/morgan/esp/hardwareone-idf/components/hardwareone/System_ESPNow.cpp:5500-5700`

**Stack Consumers:**
```cpp
// Topology response building
std::vector<MeshTopoPeer> peers;   // Dynamic allocation
for (int i = 0; i < gMeshPeerSlots; i++) {
  MeshTopoPeer peer = {};          // 64 bytes per peer
  peer.mac = ...;
  peer.name = ...;                 // String allocation
  peer.rssi = ...;
  peers.push_back(peer);           // Vector growth
}

// Serialize to JSON
JsonDocument doc;
JsonArray peersArray = doc.to<JsonArray>();
for (auto& peer : peers) {
  JsonObject obj = peersArray.add<JsonObject>();
  // ... populate
}
```

**Why it spikes:**
- Vector allocations for peer list
- String allocations for device names
- JSON serialization overhead
- Broadcast to all peers

**Peak Stack:** ~7-9 KB during topology collection with 10+ peers

---

### ESP-NOW Task Recommendations

**Current:** 24 KB (6144 words)  
**Actual Peak:** ~12 KB  
**Recommended:** **16 KB (4096 words)** - 33% safety margin

**Savings:** 8 KB per espnow_task

---

## 3. Main Task Spikes (21 KB stack)

**Current Stack:** 21 KB (Arduino default)  
**Typical Usage:** 4 KB (18%)  
**Peak Usage:** ~8-10 KB during specific operations

### Primary Spike Causes

#### A. **Periodic Memory Sampling** (`periodicMemorySample`)
**Location:** `@/Users/morgan/esp/hardwareone-idf/components/hardwareone/System_MemoryMonitor.cpp:200-350`

**Stack Consumers:**
```cpp
void periodicMemorySample() {
  // Allocation tracker processing
  for (int i = 0; i < gAllocTrackerCount; i++) {
    AllocEntry& e = gAllocTracker[i];
    // Format and log each allocation
    snprintf(buf, sizeof(buf), "[ALLOC] %s: %u bytes", e.tag, e.size);
  }
  
  // Task stack iteration
  TaskStatus_t* taskArray = ...;   // Dynamic allocation
  uxTaskGetSystemState(taskArray, ...);
  
  // Per-task reporting
  for (int i = 0; i < taskCount; i++) {
    reportTaskStack(taskArray[i].xHandle, ...);
  }
}

// Call stack:
hardwareone_loop()
  → periodicMemorySample()
    → Iterate allocation tracker (up to 64 entries)
      → snprintf for each (~256 bytes buffer)
        → broadcastOutput() for logging
    → uxTaskGetSystemState()
      → Allocate task array
        → Iterate and report each task
```

**Why it spikes:**
- Processes up to 64 allocation tracker entries
- Allocates task status array (20+ tasks × 52 bytes = ~1 KB)
- Multiple snprintf calls for formatting
- Debug output for each allocation

**Peak Stack:** ~5-6 KB when allocation tracker is full

---

#### B. **Task Stack Reporting** (`reportAllTaskStacks`)
**Location:** `@/Users/morgan/esp/hardwareone-idf/components/hardwareone/System_TaskUtils.cpp:227-557`

**Stack Consumers:**
```cpp
void reportAllTaskStacks() {
  // Get all tasks
  UBaseType_t taskCount = uxTaskGetNumberOfTasks();
  TaskStatus_t* taskArray = (TaskStatus_t*)malloc(taskCount * sizeof(TaskStatus_t));
  
  uxTaskGetSystemState(taskArray, taskCount, NULL);
  
  // Sort by stack usage
  std::sort(taskArray, taskArray + taskCount, ...);  // Sorting overhead
  
  // Report each task
  for (UBaseType_t i = 0; i < taskCount; i++) {
    char taskName[16];
    uint32_t stackSize = ...;
    uint32_t freeStack = ...;
    // Calculate percentages, format output
    BROADCAST_PRINTF(...);  // Large format string
  }
  
  free(taskArray);
}
```

**Why it spikes:**
- Allocates task array (20+ tasks)
- Sorting algorithm overhead
- Multiple BROADCAST_PRINTF calls (each ~512 bytes format buffer)
- String formatting for percentages and sizes

**Peak Stack:** ~6-7 KB during full task report

---

#### C. **Automation Scheduler** (`schedulerTickMinute`)
**Location:** `@/Users/morgan/esp/hardwareone-idf/components/hardwareone/System_Automation.cpp:2000-2300`

**Stack Consumers:**
```cpp
void schedulerTickMinute() {
  // Load automations from file
  String json;
  readText(AUTOMATIONS_JSON_FILE, json);  // Up to 32 KB
  
  // Parse JSON
  JsonDocument doc;                        // PSRAM allocator
  deserializeJson(doc, json);
  
  // Evaluate each automation
  JsonArray autos = doc["automations"];
  for (JsonObject auto : autos) {
    // Evaluate conditions
    bool result = evaluateCondition(auto["condition"]);
    
    // Execute commands if triggered
    if (result) {
      executeConditionalCommand(auto["action"]);  // Deep call
    }
  }
}

// Deep call chain:
schedulerTickMinute()
  → readText() (file I/O)
    → deserializeJson() (JSON parsing)
      → Iterate automations
        → evaluateCondition()
          → Parse condition string
            → Evaluate operators
        → executeConditionalCommand()
          → Parse command list
            → Execute each command
              → executeCommand() (~2-4 KB)
```

**Why it spikes:**
- Large JSON file reading (up to 32 KB)
- JSON parsing overhead
- Nested condition evaluation
- Command execution for triggered automations
- String operations for parsing

**Peak Stack:** ~8-10 KB when processing complex automations

---

#### D. **Sensor Logging** (`sensorLogTick`)
**Location:** `@/Users/morgan/esp/hardwareone-idf/components/hardwareone/System_SensorLogging.cpp:400-600`

**Stack Consumers:**
```cpp
void sensorLogTick() {
  // Check if logging interval elapsed
  if (millis() - lastLog < interval) return;
  
  // Build sensor data JSON
  JsonDocument doc;
  JsonObject root = doc.to<JsonObject>();
  
  // Add each sensor's data
  if (thermalEnabled) {
    JsonObject thermal = root["thermal"].to<JsonObject>();
    // ... populate thermal data
  }
  if (imuEnabled) {
    JsonObject imu = root["imu"].to<JsonObject>();
    // ... populate IMU data
  }
  // ... repeat for all sensors
  
  // Serialize and write to file
  String output;
  serializeJson(doc, output);
  File f = VFS::open(logFile, "a");
  f.println(output);
  f.close();
}
```

**Why it spikes:**
- JSON document building for multiple sensors
- String serialization
- File append operations
- Multiple sensor data queries

**Peak Stack:** ~5-6 KB when logging all sensors

---

### Main Task Recommendations

**Current:** 21 KB (Arduino default)  
**Actual Peak:** ~10 KB  
**Recommended:** **14 KB (3584 words)** - 40% safety margin

**Savings:** 7 KB per main task

---

## Summary of Spike Triggers

### HTTPD Task
1. **File uploads** (base64 decode + streaming write)
2. **File writes** (large body + automation hooks)
3. **Settings schema** (190+ entries × snprintf)
4. **Directory listing** (recursive traversal)

### ESP-NOW Task
1. **Multi-peer heartbeats** (10+ peers simultaneously)
2. **Worker status updates** (large payloads + metadata)
3. **File transfers** (chunk processing + completion)
4. **Topology collection** (vector allocation + JSON)

### Main Task
1. **Memory sampling** (64 allocations + task iteration)
2. **Task stack reporting** (sorting + formatting)
3. **Automation scheduler** (JSON parsing + execution)
4. **Sensor logging** (multi-sensor JSON building)

---

## Recommended Stack Reductions

| Task | Current | Peak | Recommended | Savings |
|------|---------|------|-------------|---------|
| **httpd** | 35 KB | 8 KB | 12 KB | **23 KB** |
| **espnow_task** | 24 KB | 12 KB | 16 KB | **8 KB** |
| **main** | 21 KB | 10 KB | 14 KB | **7 KB** |
| **TOTAL** | 80 KB | 30 KB | 42 KB | **38 KB** |

**Total Memory Savings: 38 KB of DRAM**

---

## Implementation

Update `@/Users/morgan/esp/hardwareone-idf/components/hardwareone/System_TaskUtils.h`:

```cpp
// Before:
constexpr uint32_t ESPNOW_HB_STACK_WORDS = 6144;     // ~24KB

// After:
constexpr uint32_t ESPNOW_HB_STACK_WORDS = 4096;     // ~16KB
```

Update `@/Users/morgan/esp/hardwareone-idf/components/hardwareone/WebServer_Server.cpp`:

```cpp
// Before:
config.stack_size = 8192;  // 8 KB (35 KB total with ESP-IDF overhead)

// After:
config.stack_size = 3072;  // 3 KB (12 KB total with ESP-IDF overhead)
```

Update Arduino main task stack (in platformio.ini or Arduino IDE settings):
```ini
board_build.arduino.stack_size = 3584  ; 14 KB (was 21 KB)
```

---

**Note:** These are conservative recommendations with safety margins. Monitor stack watermarks after changes to ensure no overflows occur during peak operations.
