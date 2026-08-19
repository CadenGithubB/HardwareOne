/**
 * I2C Device Manager Implementation
 * Unified I2C subsystem controller — supports up to NUM_BUSES (currently 2)
 * independent I2C buses, each with its own mutex / Wire instance / clock
 * stack / metrics. Bus 0 = Wire1 (primary STEMMA QT / "I2C1"), bus 1 = Wire
 * (secondary STEMMA QT / "I2C2", only used on boards like the FeatherS3[D]).
 */

#include <Wire.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "System_Debug.h"
#include "System_I2C_Manager.h"
#include "System_Logging.h"
#include "System_Settings.h"
#include "System_PollPause.h"   // pollPause/pollResume — global sensor-poll pause
#include "System_TaskUtils.h"   // taskStackRecord — taskstats stack-size registry

// I2C bus configuration (defaults, overridden by settings at runtime)
#define I2C_WIRE1_DEFAULT_FREQ 100000

// (Removed i2cPortForBus + the legacy i2c_filter_enable() calls: on IDF >= 5.4
// Arduino's Wire uses the new i2c_master driver, which applies the glitch
// filter itself — esp32-hal-i2c-ng.c sets bus_config.glitch_ignore_cnt = 7 in
// i2cInit(). The old legacy-driver call would conflict with the master driver.)

// Singleton instance
I2CDeviceManager* I2CDeviceManager::instance = nullptr;

// ============================================================================
// Singleton Management
// ============================================================================

I2CDeviceManager::I2CDeviceManager()
  : deviceCount(0), managerMutex(nullptr),
    queueHead(0), queueTail(0), queueMutex(nullptr), pollingPaused(false) {
  // Per-bus arrays — zero everything, wire up Arduino TwoWire pointers.
  // wires[] are set here at construction (compile-time addresses) so the
  // table is always valid even before initBus() runs; busInitialized[] is
  // the gate that prevents transactions on an un-begun bus.
  for (uint8_t b = 0; b < NUM_BUSES; b++) {
    busMutexes[b]         = nullptr;
    currentClockHz[b]     = 0;
    defaultClockHz[b]     = I2C_WIRE1_DEFAULT_FREQ;
    busInitialized[b]     = false;
    lastRecoveryMs[b]     = 0;
    clockStackDepths[b]   = 0;
    memset(&busMetrics[b], 0, sizeof(busMetrics[b]));
    memset(clockStacks[b], 0, sizeof(clockStacks[b]));
  }
  wires[0] = &Wire1;  // bus 0 = primary  → Wire1 → ESP I2C_NUM_1
  wires[1] = &Wire;   // bus 1 = secondary → Wire  → ESP I2C_NUM_0
  memset(deviceQueue, 0, sizeof(deviceQueue));
}

void I2CDeviceManager::initialize() {
  if (instance) return;

  instance = new I2CDeviceManager();
  if (!instance) {
    Serial.println("[I2C_MGR] FATAL: Failed to allocate manager");
    while(1) delay(1000);
  }

  // Create per-bus mutexes (cheap — single FreeRTOS mutex each, ~80 B).
  // Even if bus 1 is never used, the mutex sits idle costing little.
  for (uint8_t b = 0; b < NUM_BUSES; b++) {
    instance->busMutexes[b] = xSemaphoreCreateMutex();
    if (!instance->busMutexes[b]) {
      Serial.printf("[I2C_MGR] FATAL: Failed to create bus %u mutex\n", b);
      while(1) delay(1000);
    }
  }
  instance->managerMutex = xSemaphoreCreateMutex();
  instance->queueMutex   = xSemaphoreCreateMutex();

  if (!instance->managerMutex || !instance->queueMutex) {
    Serial.println("[I2C_MGR] FATAL: Failed to create manager/queue mutexes");
    while(1) delay(1000);
  }

  INFO_I2CF("Manager initialized successfully (NUM_BUSES=%u)", NUM_BUSES);
}

I2CDeviceManager* I2CDeviceManager::getInstance() {
  if (!instance) {
    initialize();
  }
  return instance;
}

// ============================================================================
// Device Registration
// ============================================================================

I2CDevice* I2CDeviceManager::registerDevice(uint8_t addr, const char* name,
                                             uint32_t clockHz, uint32_t timeoutMs,
                                             uint8_t busIdx) {
  if (!managerMutex) return nullptr;
  if (busIdx >= NUM_BUSES) busIdx = 0;  // clamp invalid bus to primary

  if (xSemaphoreTake(managerMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    // Devices are now keyed on (address, bus). Same physical address can
    // legitimately live on both buses (e.g., two DS3231 RTCs, one per bus)
    // and each gets its own I2CDevice slot with independent health tracking.
    for (int i = 0; i < deviceCount; i++) {
      if (devices[i].address == addr && devices[i].bus == busIdx) {
        // Update name if upgrading from "Auto" to a real name
        if (strcmp(devices[i].name, "Auto") == 0 && strcmp(name, "Auto") != 0) {
          devices[i].name = name;
          devices[i].clockHz = clockHz;
          // Set BOTH: this upgrade is the driver declaring the device's real
          // configured timeout, which is by definition the new base. Setting
          // only the adaptive value would leave baseTimeoutMs holding the
          // value from the anonymous auto-registration, and the decay in
          // recordSuccess() would then drag the timeout back down to that
          // stale figure on the next successful transaction.
          devices[i].baseTimeoutMs = timeoutMs;
          devices[i].adaptiveTimeoutMs = timeoutMs;
          INFO_I2CF("Updated device 0x%02X bus=%u: Auto -> %s clock=%luHz timeout=%lums",
                    addr, busIdx, name, (unsigned long)clockHz, (unsigned long)timeoutMs);
        }
        xSemaphoreGive(managerMutex);
        return &devices[i];
      }
    }

    if (deviceCount >= MAX_DEVICES) {
      ERROR_I2CF("Cannot register 0x%02X bus=%u - max devices reached", addr, busIdx);
      xSemaphoreGive(managerMutex);
      return nullptr;
    }

    I2CDevice* dev = &devices[deviceCount++];
    dev->init(addr, name, clockHz, timeoutMs, busIdx);

    INFO_I2CF("Registered device 0x%02X (%s) bus=%u clock=%luHz timeout=%lums",
              addr, name, busIdx, (unsigned long)clockHz, (unsigned long)timeoutMs);

    xSemaphoreGive(managerMutex);
    return dev;
  }

  return nullptr;
}

I2CDevice* I2CDeviceManager::getDevice(uint8_t addr, uint8_t busIdx) {
  if (busIdx >= NUM_BUSES) busIdx = 0;
  for (int i = 0; i < deviceCount; i++) {
    if (devices[i].address == addr && devices[i].bus == busIdx) {
      return &devices[i];
    }
  }
  return nullptr;
}

I2CDevice* I2CDeviceManager::getDeviceAnyBus(uint8_t addr) {
  // Buses in ascending order so a dual-registered address resolves to bus 0
  // deterministically (registration order in devices[] is not bus-ordered).
  for (uint8_t b = 0; b < NUM_BUSES; b++) {
    for (int i = 0; i < deviceCount; i++) {
      if (devices[i].address == addr && devices[i].bus == b) {
        return &devices[i];
      }
    }
  }
  return nullptr;
}

I2CDevice* I2CDeviceManager::getDeviceByName(const char* name) {
  if (!name) return nullptr;
  
  for (int i = 0; i < deviceCount; i++) {
    if (devices[i].name && strcmp(devices[i].name, name) == 0) {
      return &devices[i];
    }
  }
  return nullptr;
}

// ============================================================================
// Bus Operations
// ============================================================================

// Resolve the SDA/SCL pin pair for a given bus from settings. Bus 0 uses the
// primary I2C pins (gSettings.i2cSdaPin/SclPin); bus 1 uses the secondary
// (gSettings.i2c2SdaPin/SclPin). Returns false if the pins are unavailable
// (e.g., -1 on a board without a second port).
static bool busPinsFromSettings(uint8_t bus, int* sda, int* scl) {
  if (bus == 0) {
    *sda = gSettings.i2cSdaPin;
    *scl = gSettings.i2cSclPin;
  } else {
    *sda = gSettings.i2c2SdaPin;
    *scl = gSettings.i2c2SclPin;
  }
  return (*sda >= 0 && *scl >= 0);
}

// ---------------------------------------------------------------------------
// Force bus 0's I2C interrupt onto CPU1 (off the BLE controller's core)
// ---------------------------------------------------------------------------
// The legacy ESP-IDF I2C driver allocates its ISR with esp_intr_alloc(), which
// pins the interrupt to whichever core calls i2c_driver_install() — and that
// happens inside Arduino's TwoWire::begin() (via i2cInit). At boot, begin()
// runs on the main task, which lives on CPU0, the same core as the Bluedroid
// BLE controller (btdm_controller_task). Under RF coexistence a glitchy
// transaction on the gamepad bus (Wire1 / I2C_NUM_1) makes the legacy driver
// re-arm its command from inside the ISR — an interrupt storm that monopolizes
// CPU0 and trips the interrupt watchdog (Int WDT on CPU0). Running begin() on
// CPU1 moves that ISR off the BLE core so the storm can no longer starve it.
//
// TwoWire::begin() guards on `if (i2cIsInit(num))` — once we install on CPU1
// here, every later begin() (e.g. Adafruit_seesaw's own) is a no-op and won't
// drag the ISR back to CPU0. The only path that re-installs is the explicit
// end()+begin() in performBusRecovery(), which routes through here too.
//
// esp_ipc_call_blocking() isn't usable: the IPC task stack is only 1024 B and
// begin()/i2c_driver_install() needs more headroom. Spawn a short-lived task
// pinned to CPU1 with a real stack, run begin() on it, then let it self-delete.
namespace {
struct BusBeginReq {
  TwoWire* wire;
  int sda;
  int scl;
  SemaphoreHandle_t done;
};
void busBeginTask(void* arg) {
  BusBeginReq* req = static_cast<BusBeginReq*>(arg);
  req->wire->begin(req->sda, req->scl);
  xSemaphoreGive(req->done);   // begin() complete; req is no longer touched
  vTaskDelete(nullptr);
}
}  // namespace

// Run wire->begin(sda, scl) on CPU1 and block until it finishes. Falls back to
// a direct begin on the current core if the helper task can't be created.
static void beginBusOnCpu1(TwoWire* wire, int sda, int scl) {
  SemaphoreHandle_t done = xSemaphoreCreateBinary();
  if (!done) { wire->begin(sda, scl); return; }
  BusBeginReq req{ wire, sda, scl, done };
  TaskHandle_t th = nullptr;
  taskStackRecord("i2c0_begin", 4096);
  BaseType_t ok = xTaskCreatePinnedToCore(busBeginTask, "i2c0_begin", 4096,
                                          &req, 10, &th, 1 /* CPU1 */);
  if (ok != pdPASS) {
    vSemaphoreDelete(done);
    wire->begin(sda, scl);     // fallback: begin on the current core
    return;
  }
  xSemaphoreTake(done, portMAX_DELAY);   // req must outlive the task
  vSemaphoreDelete(done);
}

void I2CDeviceManager::initBus(uint8_t busIdx, int sdaPin, int sclPin, uint32_t hz) {
  if (busIdx >= NUM_BUSES) return;
  if (sdaPin < 0 || sclPin < 0) {
    INFO_I2CF("initBus skipped: bus %u pins invalid (sda=%d scl=%d)", busIdx, sdaPin, sclPin);
    return;
  }
  TwoWire* wire = wires[busIdx];
  if (!wire) return;

  // Power-gating: bus 1 on some boards (FeatherS3[D]) is powered through a
  // switchable LDO whose enable line is exposed as I2C2_POWER_PIN. Drive it
  // HIGH here, independent of NeoPixel state — previously this was a hidden
  // dependency where the bus only worked if the NeoPixel driver had run
  // first to assert the same physical pin. Now bus 1 owns the assertion
  // for the I2C2 role and the NeoPixel still asserts it for the LED role;
  // either alone keeps the rail up, both is idempotent.
  if (busIdx == 1) {
#if defined(I2C2_POWER_PIN) && (I2C2_POWER_PIN >= 0)
    pinMode(I2C2_POWER_PIN, OUTPUT);
    digitalWrite(I2C2_POWER_PIN, HIGH);
    delay(5);  // brief settle so the LDO is stable before we begin clocking
    INFO_I2CF("I2C2 power pin GPIO%d asserted HIGH (LDO2 enable)", (int)I2C2_POWER_PIN);
#endif
  }

  if (busIdx == 0) {
    // Gamepad / Seesaw bus: install its ISR on CPU1, away from the BLE
    // controller on CPU0 (see beginBusOnCpu1 for the coexistence rationale).
    beginBusOnCpu1(wire, sdaPin, sclPin);
  } else {
    wire->begin(sdaPin, sclPin);
  }
  wire->setClock(hz);
  // 100ms TwoWire-level timeout — well under CONFIG_ESP_INT_WDT_TIMEOUT_MS
  // (1500ms) so a hung transaction won't trigger the interrupt watchdog.
  wire->setTimeOut(100);
  currentClockHz[busIdx] = hz;
  defaultClockHz[busIdx] = hz;
  busInitialized[busIdx] = true;

  // Glitch filter (ignore pulses < 7 APB cycles, ~88ns @ 80MHz; prevents EMI
  // spurious bus errors) is now applied by the i2c_master driver under Wire —
  // esp32-hal-i2c-ng.c sets glitch_ignore_cnt=7 in i2cInit(). No manual call.

  INFO_I2CF("Bus %u initialized: %s (SDA=%d, SCL=%d, %lu Hz)",
            busIdx, (busIdx == 0) ? "Wire1/I2C1" : "Wire/I2C2",
            sdaPin, sclPin, (unsigned long)hz);
  logSystemEvent("I2C", "bus %u online: %s (SDA=%d SCL=%d @ %lu Hz)",
                 busIdx, (busIdx == 0) ? "I2C1" : "I2C2",
                 sdaPin, sclPin, (unsigned long)hz);
}

void I2CDeviceManager::initBuses() {
  // Bus 0 (primary) — always initialized when I2C is enabled. The caller
  // (initI2CBuses in System_I2C.cpp) already gated on gI2CBusRunning.
  int sda0, scl0;
  if (busPinsFromSettings(0, &sda0, &scl0)) {
    initBus(0, sda0, scl0, I2C_WIRE1_DEFAULT_FREQ);
  } else {
    WARN_I2CF("Bus 0 pins not configured (sda=%d scl=%d) — primary I2C unavailable",
              sda0, scl0);
    logSystemEvent("I2C", "bus 0 init FAILED (pins sda=%d scl=%d) — every device on the primary bus is unavailable",
                   sda0, scl0);
  }

  // Bus 1 (secondary) — only when the user explicitly enabled it AND the
  // board defines valid pins. Skips silently otherwise (no error spam).
  if (gSettings.i2c2Enabled) {
    int sda1, scl1;
    if (busPinsFromSettings(1, &sda1, &scl1)) {
      initBus(1, sda1, scl1, I2C_WIRE1_DEFAULT_FREQ);
    } else {
      WARN_I2CF("i2c2BusEnabled but pins invalid (sda=%d scl=%d) — bus 1 NOT initialized; "
                "check I2C2 settings or use a board with a second port (e.g., FeatherS3[D])",
                sda1, scl1);
      // Symmetric with the bus-0 failure event: a user enabled I2C2 but its pins
      // are invalid, so every device on the secondary bus is silently unavailable.
      logSystemEvent("I2C", "bus 1 init FAILED (i2c2 enabled but pins sda=%d scl=%d invalid) — secondary bus unavailable",
                     sda1, scl1);
    }
  }

  delay(100);
}

void I2CDeviceManager::performBusRecovery(uint8_t busIdx) {
  if (busIdx >= NUM_BUSES) busIdx = 0;
  if (!busInitialized[busIdx]) {
    WARN_I2CF("Bus recovery skipped: bus %u not initialized", busIdx);
    return;
  }
  TwoWire* wire = wires[busIdx];
  SemaphoreHandle_t mutex = busMutexes[busIdx];
  int sdaPin, sclPin;
  if (!busPinsFromSettings(busIdx, &sdaPin, &sclPin)) {
    ERROR_I2CF("Bus recovery skipped: bus %u pins invalid", busIdx);
    return;
  }

  WARN_I2CF("Performing bus %u recovery (SDA=%d SCL=%d)", busIdx, sdaPin, sclPin);

  // Pause polling for THIS bus only — recovery tears down and re-begins just
  // this bus's Wire, so only sensors on it must hold off; the other bus keeps
  // running. Uses the per-bus primitive directly (not pausePolling(), which is
  // the blanket method that also drives the i2cpause/i2cresume CLI latch — we
  // must not disturb that here). Ref-counted, so pair unconditionally.
  pollPause(busIdx);

  bool locked = (mutex && xSemaphoreTake(mutex, pdMS_TO_TICKS(2000)) == pdTRUE);
  if (!locked) {
    pollResume(busIdx);
    ERROR_I2CF("Bus %u recovery failed - couldn't acquire mutex", busIdx);
    return;
  }

  // 1. End the bus session
  wire->end();
  delay(10);

  // 2. Manual clock toggle to release any device stuck mid-byte
  pinMode(sclPin, OUTPUT);
  pinMode(sdaPin, INPUT_PULLUP);

  for (int i = 0; i < 9; i++) {
    digitalWrite(sclPin, LOW);
    delayMicroseconds(5);
    digitalWrite(sclPin, HIGH);
    delayMicroseconds(5);

    if (digitalRead(sdaPin)) {
      INFO_I2CF("Bus %u SDA released after %d clock pulses", busIdx, i + 1);
      break;
    }
  }

  // 3. Generate STOP condition
  pinMode(sdaPin, OUTPUT);
  digitalWrite(sdaPin, LOW);
  delayMicroseconds(5);
  digitalWrite(sclPin, HIGH);
  delayMicroseconds(5);
  digitalWrite(sdaPin, HIGH);
  delayMicroseconds(5);

  // 4. Reinitialize this bus. Bus 0 re-installs its ISR on CPU1 (the end()
  //    above freed it); keep it off the BLE core across recovery too.
  if (busIdx == 0) {
    beginBusOnCpu1(wire, sdaPin, sclPin);
  } else {
    wire->begin(sdaPin, sclPin);
  }
  wire->setClock(defaultClockHz[busIdx]);
  currentClockHz[busIdx] = defaultClockHz[busIdx];
  // Glitch filter re-applied automatically by the i2c_master driver on the
  // wire->begin() above (esp32-hal-i2c-ng.c, glitch_ignore_cnt=7).
  delay(50);

  // 5. Reset health for devices on THIS bus only — devices on the other bus
  //    are unaffected by recovery here.
  for (int i = 0; i < deviceCount; i++) {
    if (devices[i].bus == busIdx) {
      devices[i].health.consecutiveErrors = 0;
      devices[i].health.degraded = false;
    }
  }

  xSemaphoreGive(mutex);

  pollResume(busIdx);

  // Stamp last so the cooldown measures from completion, not from entry.
  lastRecoveryMs[busIdx] = millis();

  INFO_I2CF("Bus %u recovery complete", busIdx);
}

// Minimum gap between two error-driven recoveries of the same bus. The
// BUS_ERROR path recovers on a single event with no threshold (unlike the
// NACK/TIMEOUT paths, which need 3 consecutive failures first), so without a
// cooldown a genuinely broken bus would recover in a tight loop — each attempt
// pausing polling, tearing down Wire, and bit-banging 9 clock pulses.
static const uint32_t BUS_RECOVERY_COOLDOWN_MS = 5000;

bool I2CDeviceManager::busRecoveryThrottled(uint8_t busIdx) const {
  if (busIdx >= NUM_BUSES) return false;
  if (lastRecoveryMs[busIdx] == 0) return false;   // never recovered yet
  return (millis() - lastRecoveryMs[busIdx]) < BUS_RECOVERY_COOLDOWN_MS;
}

uint8_t I2CDeviceManager::probeDeviceStatus(uint8_t bus, uint8_t addr) {
  if (bus >= NUM_BUSES || !wires[bus] || !busInitialized[bus]) return 4;
  // Address-only transaction: 9 clock bits, no payload. Cheap enough to run on
  // the failure path, and it answers exactly the question the classifier needs
  // — does this device still ACK its address, or is the bus itself unhappy?
  wires[bus]->beginTransmission(addr);
  return wires[bus]->endTransmission(true);
}

void I2CDeviceManager::checkBusRecoveryNeeded() {
  // Evaluate each bus independently. Each has its own mutex, Wire instance and
  // clock stack, so a degradation cascade on one says nothing about the other.
  // Before this was made bus-aware it counted every device in the registry and
  // then always recovered bus 0 (performBusRecovery's default arg) — a bus 1
  // cascade would tear down the healthy primary bus and leave the sick one
  // untouched.
  for (uint8_t b = 0; b < NUM_BUSES; b++) {
    if (!busInitialized[b]) continue;

    int total = 0;
    int degradedCount = 0;
    for (int i = 0; i < deviceCount; i++) {
      if (!devices[i].isInitialized() || devices[i].bus != b) continue;
      total++;
      if (devices[i].isDegraded()) degradedCount++;
    }
    if (total == 0 || degradedCount == 0) continue;

    float degradationPercent = (degradedCount * 100.0f) / total;

    // Quorum floor. The >66% rule assumes a populated bus: with the registry
    // now split per bus, a secondary bus holding one or two devices would hit
    // 100% on a single degraded device and trigger a full bus teardown — far
    // more trigger-happy than the whole-registry behavior this replaces. A
    // lone sick device is already covered by its own degrade / 30s-retry
    // cycle, so require a real quorum before escalating to the bus.
    if (total < 3 || degradedCount < 2) {
      INFO_I2CF("Bus %u health: %d/%d degraded (%.1f%%) - below quorum, no bus recovery",
                b, degradedCount, total, degradationPercent);
      continue;
    }

    // Trigger bus recovery if >66% degraded (2/3 threshold)
    if (degradationPercent > 66.0f) {
      ERROR_I2CF("CRITICAL: bus %u has %d/%d devices degraded (%.1f%%) - triggering bus recovery",
                 b, degradedCount, total, degradationPercent);
      performBusRecovery(b);
    } else {
      INFO_I2CF("Bus %u health: %d/%d devices degraded (%.1f%%) - recovery threshold not reached",
                b, degradedCount, total, degradationPercent);
    }
  }
}

// ============================================================================
// Clock Management
// ============================================================================

bool I2CDeviceManager::clockStackPush(uint8_t bus, uint32_t hz) {
  if (bus >= NUM_BUSES) return false;
  if (clockStackDepths[bus] >= CLOCK_STACK_MAX) {
    // IMPORTANT: do NOT use snprintf+char buffer here. clockStackPush is on
    // the hot path (every I2C transaction); GCC eagerly reserves the full
    // function frame at entry, so any local char[N] adds N bytes to EVERY
    // call, even when this overflow branch isn't taken. An 80-byte buffer
    // was enough to push sensor_queue_task (11 KB stack) over during
    // seesaw.begin() — which goes ~10 transactions deep. Surrounding log
    // lines already identify the bus, so the literal string is fine here.
    broadcastOutput("[I2C_MGR] CRITICAL: clock stack overflow - operation aborted");
    return false;
  }
  clockStacks[bus][clockStackDepths[bus]++] = hz;
  return true;
}

void I2CDeviceManager::clockStackPop(uint8_t bus) {
  if (bus >= NUM_BUSES) return;
  if (clockStackDepths[bus] > 0) {
    clockStackDepths[bus]--;
  }
}

uint32_t I2CDeviceManager::clockStackTopOrDefault(uint8_t bus) {
  if (bus >= NUM_BUSES) return I2C_WIRE1_DEFAULT_FREQ;
  return (clockStackDepths[bus] > 0)
           ? clockStacks[bus][clockStackDepths[bus] - 1]
           : defaultClockHz[bus];
}

void I2CDeviceManager::setBusClock(uint8_t bus, uint32_t hz) {
  if (bus >= NUM_BUSES || !wires[bus]) return;
  if (currentClockHz[bus] != hz) {
    wires[bus]->setClock(hz);
    currentClockHz[bus] = hz;
    delayMicroseconds(50);
  }
}

// ============================================================================
// Metrics Tracking
// ============================================================================

void I2CDeviceManager::updateMetrics(uint8_t bus, uint32_t waitUs, uint32_t txDurationUs, uint32_t clockHz) {
  if (bus >= NUM_BUSES) return;
  I2CBusMetrics& m = busMetrics[bus];

  // Mutex wait metrics
  if (waitUs > 0) m.mutexContentions++;
  if (waitUs > m.maxWaitTimeUs) m.maxWaitTimeUs = waitUs;
  m.avgWaitTimeUs = (m.avgWaitTimeUs * 7 + waitUs) / 8;

  // Transaction duration metrics
  if (txDurationUs > m.maxTransactionDurationUs) {
    m.maxTransactionDurationUs = txDurationUs;
  }
  m.avgTransactionDurationUs = (m.avgTransactionDurationUs * 7 + txDurationUs) / 8;

  // Estimate bytes transferred (rough; assumes 8 bit/byte + 1 ACK bit)
  uint32_t estimatedBytes = (txDurationUs * clockHz) / (8 * 1000000);
  if (estimatedBytes > 0) {
    m.totalBytesTransferred += estimatedBytes;
  }

  updateHistogram(bus, txDurationUs);
}

void I2CDeviceManager::updateHistogram(uint8_t bus, uint32_t txDurationUs) {
  if (bus >= NUM_BUSES) return;
  I2CBusMetrics& m = busMetrics[bus];
  if (txDurationUs < 100) {
    m.txDuration_0_100us++;
  } else if (txDurationUs < 500) {
    m.txDuration_100_500us++;
  } else if (txDurationUs < 2000) {
    m.txDuration_500_2000us++;
  } else {
    m.txDuration_2000plus_us++;
  }
}


// ============================================================================
// I2C Device Lifecycle Management
// ============================================================================

bool I2CDeviceManager::enqueueDeviceStart(I2CDeviceType sensor) {
  // Refuse to enqueue if the bus is disabled — nothing will drain the queue
  extern bool gI2CBusRunning;
  if (!gI2CBusRunning) return false;

  if (!queueMutex) return false;
  
  if (xSemaphoreTake(queueMutex, portMAX_DELAY) == pdTRUE) {
    int nextTail = (queueTail + 1) % 8;
    if (nextTail == queueHead) {
      xSemaphoreGive(queueMutex);
      return false;  // Queue full
    }
    
    deviceQueue[queueTail].device = sensor;
    deviceQueue[queueTail].queuedAt = millis();
    queueTail = nextTail;
    
    xSemaphoreGive(queueMutex);
    return true;
  }
  
  return false;
}

bool I2CDeviceManager::dequeueDeviceStart(I2CDeviceStartRequest* req) {
  if (!queueMutex || !req) return false;
  
  if (xSemaphoreTake(queueMutex, portMAX_DELAY) == pdTRUE) {
    if (queueHead == queueTail) {
      xSemaphoreGive(queueMutex);
      return false;  // Queue empty
    }
    
    *req = deviceQueue[queueHead];
    queueHead = (queueHead + 1) % 8;
    
    xSemaphoreGive(queueMutex);
    return true;
  }
  
  return false;
}

bool I2CDeviceManager::isInQueue(I2CDeviceType sensor) {
  if (!queueMutex) return false;
  
  bool found = false;
  if (xSemaphoreTake(queueMutex, portMAX_DELAY) == pdTRUE) {
    for (int i = queueHead; i != queueTail; i = (i + 1) % 8) {
      if (deviceQueue[i].device == sensor) {
        found = true;
        break;
      }
    }
    xSemaphoreGive(queueMutex);
  }
  
  return found;
}

int I2CDeviceManager::getQueuePosition(I2CDeviceType sensor) {
  if (!queueMutex) return -1;
  
  int pos = -1;
  if (xSemaphoreTake(queueMutex, portMAX_DELAY) == pdTRUE) {
    int idx = 1;
    for (int i = queueHead; i != queueTail; i = (i + 1) % 8) {
      if (deviceQueue[i].device == sensor) {
        pos = idx;
        break;
      }
      idx++;
    }
    xSemaphoreGive(queueMutex);
  }
  
  return pos;
}

int I2CDeviceManager::getQueueDepth() {
  return (queueTail - queueHead + 8) % 8;
}

// These delegate to the global pollPause/pollResume (System_PollPause.h) but
// also track the manager's own `pollingPaused` latch, which isPollingPaused()
// exposes so the i2cpause/i2cresume CLI commands stay idempotent (a double
// i2cpause must not push the global depth counter out of balance).
void I2CDeviceManager::pausePolling() {
  pollingPaused = true;
  pollPause();
  INFO_I2CF("Sensor polling paused");
}

void I2CDeviceManager::resumePolling() {
  pollingPaused = false;
  pollResume();
  INFO_I2CF("Sensor polling resumed");
}

// ============================================================================
// Device Discovery
// ============================================================================

void I2CDeviceManager::discoverDevices() {
  INFO_I2CF("Starting device discovery");
  
  // Discovery implementation will scan registered device addresses
  // For now, devices are pre-registered from database in initI2CManager()
  // Future: Add runtime scanning capability
  
  INFO_I2CF("Discovery complete - %d devices registered", deviceCount);
}

// ============================================================================
// Metrics Reset
// ============================================================================


// ============================================================================
// I2CDevice Implementation (merged from System_I2C_Device.cpp)
// ============================================================================

// Maps a TwoWire::endTransmission() status to an error class.
//
// The status set is core-specific, so this table is written against the
// arduino-esp32 build we actually compile with (IDF >= 5.4, i2c_master
// driver). Wire.cpp collapses esp_err_t into exactly four values:
//   ESP_OK                        -> 0
//   ESP_FAIL / ESP_ERR_NOT_FOUND  -> 2   (no ACK: device absent or silent)
//   ESP_ERR_TIMEOUT               -> 5   (bus timeout)
//   anything else                 -> 4   (driver-level failure)
//
// The previous table assumed generic Arduino AVR semantics (3 = NACK on data,
// 4 = buffer overflow) and additionally tried to match raw esp_err_t values
// 0x103/0x107 through a uint8_t parameter, which can never hold them — the
// compiler flagged both as unreachable. The practical consequence was that 5,
// the only timeout this core emits, fell through to `default` and a plain
// timeout was escalated to BUS_ERROR, which forces an immediate bus recovery.
I2CErrorType classifyI2CError(uint8_t wireStatus) {
  switch (wireStatus) {
    case 0:
      return I2CErrorType::NONE;
    case 1:
      // "Data too long for buffer" in the Arduino contract. This core never
      // returns it, but the value is kept mapped for any caller handing us a
      // status from a stock Arduino Wire implementation.
      return I2CErrorType::BUFFER_OVERFLOW;
    case 2:
      return I2CErrorType::NACK;
    case 5:
      return I2CErrorType::TIMEOUT;
    case 4:
    default:
      // Driver-level failure (bus in a bad state, slave mode, null buffer) —
      // not a simple device NACK, so treat it as a bus-level problem.
      return I2CErrorType::BUS_ERROR;
  }
}

I2CDevice::I2CDevice()
  : address(0), bus(0), name(nullptr), clockHz(100000),
    baseTimeoutMs(200), adaptiveTimeoutMs(200) {
  memset(&health, 0, sizeof(health));
}

void I2CDevice::init(uint8_t addr, const char* deviceName, uint32_t clock, uint32_t timeout, uint8_t busIdx) {
  address = addr;
  bus = (busIdx < I2CDeviceManager::NUM_BUSES) ? busIdx : 0;
  name = deviceName;
  clockHz = clock > 0 ? clock : 100000;
  baseTimeoutMs = timeout > 0 ? timeout : 200;
  adaptiveTimeoutMs = baseTimeoutMs;
  
  // Initialize health
  health.consecutiveErrors = 0;
  health.totalErrors = 0;
  health.degraded = false;
  health.lastErrorTime = 0;
  health.lastSuccessTime = 0;
  health.registrationTime = millis();
  health.nackCount = 0;
  health.timeoutCount = 0;
  health.busErrorCount = 0;
  health.lastErrorType = I2CErrorType::NONE;
}

void I2CDevice::recordSuccess() {
  health.consecutiveErrors = 0;
  health.lastSuccessTime = millis();

  // Decay the adaptive timeout back toward its configured base, mirroring the
  // doubling in recordError()'s TIMEOUT branch. Without this the timeout only
  // ever ratchets up: a single transient storm leaves the device permanently
  // holding up to a 5s timeout, and since executeTransaction passes it to
  // xSemaphoreTake() the calling task then blocks that long on a busy bus —
  // while every health field still reads OK. Halved rather than snapped back
  // so a genuinely slow device settles at the timeout it actually needs
  // instead of oscillating between base and 2x base.
  if (adaptiveTimeoutMs > baseTimeoutMs) {
    adaptiveTimeoutMs = max(baseTimeoutMs, adaptiveTimeoutMs / 2);
  }

  if (health.degraded) {
    health.degraded = false;
    INFO_I2CF("Device 0x%02X (%s) recovered", address, name);
    logI2CRecovery(address, name, health.totalErrors);
  }
}

void I2CDevice::recordError(I2CErrorType errorType, uint8_t espError) {
  health.consecutiveErrors++;
  health.totalErrors++;
  health.lastErrorTime = millis();
  health.lastErrorType = errorType;
  
  // Type-specific error tracking and recovery
  switch (errorType) {
    case I2CErrorType::NACK:
      health.nackCount++;
      WARN_I2CF("Device 0x%02X (%s) NACK (count=%d, consecutive=%d)",
                address, name, health.nackCount, health.consecutiveErrors);
      
      if (health.consecutiveErrors >= 3) {
        health.degraded = true;
        ERROR_I2CF("Device 0x%02X (%s) marked DEGRADED after %d NACKs",
                   address, name, health.nackCount);
        logI2CError(address, name, health.consecutiveErrors, health.totalErrors, true);
        
        // Check if bus recovery is needed (decentralized check).
        // Note: checkBusRecoveryNeeded currently evaluates the WHOLE device
        // table; with dual-bus it may decide to recover, defaulting to
        // bus 0. If only this device's bus is sick, the BUS_ERROR branch
        // below routes recovery to the right bus directly.
        I2CDeviceManager::getInstance()->checkBusRecoveryNeeded();
      }
      break;
      
    case I2CErrorType::TIMEOUT:
      health.timeoutCount++;
      WARN_I2CF("Device 0x%02X (%s) TIMEOUT (count=%d, consecutive=%d)",
                address, name, health.timeoutCount, health.consecutiveErrors);
      
      // Adaptive timeout increase
      if (adaptiveTimeoutMs < 5000) {
        uint32_t oldTimeout = adaptiveTimeoutMs;
        adaptiveTimeoutMs = min(adaptiveTimeoutMs * 2, (uint32_t)5000);
        INFO_I2CF("Device 0x%02X (%s) timeout increased: %lu -> %lu ms",
                  address, name, (unsigned long)oldTimeout, 
                  (unsigned long)adaptiveTimeoutMs);
      }
      
      if (health.consecutiveErrors >= 3) {
        health.degraded = true;
        ERROR_I2CF("Device 0x%02X (%s) marked DEGRADED after %d timeouts",
                   address, name, health.timeoutCount);
        logI2CError(address, name, health.consecutiveErrors, health.totalErrors, true);
        
        // Check if bus recovery is needed (decentralized check).
        // Note: checkBusRecoveryNeeded currently evaluates the WHOLE device
        // table; with dual-bus it may decide to recover, defaulting to
        // bus 0. If only this device's bus is sick, the BUS_ERROR branch
        // below routes recovery to the right bus directly.
        I2CDeviceManager::getInstance()->checkBusRecoveryNeeded();
      }
      break;
      
    case I2CErrorType::BUS_ERROR:
      health.busErrorCount++;
      ERROR_I2CF("Device 0x%02X (%s) bus=%u BUS_ERROR (count=%d, espErr=0x%02X)",
                 address, name, bus, health.busErrorCount, espError);

      logI2CError(address, name, health.consecutiveErrors, health.totalErrors, false);

      // Trigger immediate bus recovery on THIS device's bus (was hardcoded
      // to bus 0 before dual-bus). A bus 1 device with a BUS_ERROR now
      // recovers bus 1 only; bus 0 devices keep running uninterrupted.
      //
      // Cooldown: this branch escalates on a single event with no consecutive
      // -failure threshold, so a persistently broken bus would otherwise
      // recover on every poll. Suppressed attempts are logged rather than
      // dropped silently, so the pattern is still visible in the log.
      {
        I2CDeviceManager* mgr = I2CDeviceManager::getInstance();
        if (mgr->busRecoveryThrottled(bus)) {
          WARN_I2CF("Device 0x%02X (%s) BUS_ERROR on bus %u - recovery suppressed (cooldown)",
                    address, name, bus);
        } else {
          mgr->performBusRecovery(bus);
        }
      }
      break;
      
    case I2CErrorType::BUFFER_OVERFLOW:
      ERROR_I2CF("Device 0x%02X (%s) BUFFER_OVERFLOW (espErr=0x%02X)",
                 address, name, espError);
      logI2CError(address, name, 0, health.totalErrors, false);
      break;
      
    case I2CErrorType::NONE:
      break;
  }
}

bool I2CDevice::isDegraded() const {
  if (!health.degraded) return false;
  
  // Auto-recovery after timeout
  const uint32_t RECOVERY_TIMEOUT_MS = 30000;
  if (millis() - health.lastErrorTime > RECOVERY_TIMEOUT_MS) {
    return false;  // Allow retry
  }
  
  return true;
}

void I2CDevice::attemptRecovery() {
  if (!health.degraded) return;
  
  INFO_I2CF("Device 0x%02X (%s) attempting recovery", address, name);
  health.degraded = false;
  health.consecutiveErrors = 0;
}

void I2CDevice::resetGracePeriod() {
  health.registrationTime = millis();
  health.consecutiveErrors = 0;
  health.degraded = false;
  INFO_I2CF("Device 0x%02X (%s) grace period reset", address, name);
}
