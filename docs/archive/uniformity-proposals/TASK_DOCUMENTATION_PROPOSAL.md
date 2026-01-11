# Task Function Documentation - Resolution Proposal

## Current State Analysis

Analyzed **7 sensor task functions** and found significant inconsistencies in:
1. Startup logging
2. Header documentation
3. Stack monitoring patterns
4. Cleanup code structure

---

## Current Patterns

### Pattern A: Detailed Logging (IMU, Thermal)
```cpp
void imuTask(void* parameter) {
  Serial.println("[MODULAR] imuTask() running from Sensor_IMU_BNO055.cpp");
  DEBUG_FRAMEF("IMU task started");
  unsigned long lastIMURead = 0;
  unsigned long lastStackLog = 0;
  
  while (true) {
    if (!imuEnabled) {
      // Cleanup with mutex protection
      if (i2cMutex && xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        imuConnected = false;
        if (bno != nullptr) {
          delete bno;
          bno = nullptr;
        }
        xSemaphoreGive(i2cMutex);
      }
      imuTaskHandle = nullptr;
      vTaskDelete(nullptr);
    }
    
    // Main loop...
    
    // Stack monitoring (every 60s)
    if (millis() - lastStackLog > 60000) {
      UBaseType_t watermark = uxTaskGetStackHighWaterMark(nullptr);
      DEBUG_FRAMEF("IMU task stack watermark: %lu", (unsigned long)watermark);
      lastStackLog = millis();
    }
  }
}
```

### Pattern B: Minimal Logging (ToF, GPS, Gamepad)
```cpp
void tofTask(void* parameter) {
  // No startup logging
  unsigned long lastToFRead = 0;
  
  while (true) {
    if (!tofEnabled) {
      // Cleanup
      tofTaskHandle = nullptr;
      vTaskDelete(nullptr);
    }
    // Main loop...
    // No stack monitoring
  }
}
```

### Pattern C: Mixed (APDS, FM Radio)
```cpp
void apdsTask(void* parameter) {
  Serial.println("[MODULAR] apdsTask() running from Sensor_APDS_APDS9960.cpp");
  DEBUG_FRAMEF("APDS task started");
  // Has startup logging but inconsistent cleanup
}
```

---

## Proposed Standard Pattern

### Complete Template
```cpp
// ============================================================================
// <Module> Task - FreeRTOS Task Function
// ============================================================================
// Purpose: <Brief description of what this task does>
// Stack: <size> words (~<KB> bytes) | Priority: <n> | Core: Any
// Lifecycle: Created by start<Module>SensorInternal(), deleted when <module>Enabled=false
// Polling: Every <interval>ms | I2C Clock: <speed> Hz
//
// Cleanup Strategy:
//   1. Check <module>Enabled flag
//   2. Acquire i2cMutex to prevent race conditions
//   3. Delete sensor object and clear cache
//   4. Release mutex and delete task
// ============================================================================

void <module>Task(void* parameter) {
  // Startup logging
  INFO_SENSORSF("[<Module>] Task started (handle=%p, stack=%u words)", 
                <module>TaskHandle, <STACK_SIZE>);
  
  // Task-local variables
  unsigned long lastRead = 0;
  unsigned long lastStackLog = 0;
  bool initWatermarkLogged = false;
  
  while (true) {
    // ========================================================================
    // Graceful Shutdown Check (CRITICAL - must be first)
    // ========================================================================
    if (!<module>Enabled) {
      INFO_SENSORSF("[<Module>] Task shutdown requested");
      
      // Cleanup with mutex protection to prevent race conditions
      if (<module>Connected || <sensor> != nullptr) {
        if (i2cMutex && xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
          // Safe to cleanup - no other tasks can access sensor
          <module>Connected = false;
          
          if (<sensor> != nullptr) {
            delete <sensor>;
            <sensor> = nullptr;
          }
          
          // Invalidate cache
          if (g<Module>Cache.mutex && xSemaphoreTake(g<Module>Cache.mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            g<Module>Cache.dataValid = false;
            // Clear other cache fields...
            xSemaphoreGive(g<Module>Cache.mutex);
          }
          
          xSemaphoreGive(i2cMutex);
          INFO_SENSORSF("[<Module>] Cleanup complete");
        } else {
          WARN_SENSORSF("[<Module>] Failed to acquire mutex for cleanup");
        }
      }
      
      // Final cleanup
      <module>TaskHandle = nullptr;
      INFO_SENSORSF("[<Module>] Task deleted");
      vTaskDelete(nullptr);
    }
    
    // ========================================================================
    // Polling Pause Check
    // ========================================================================
    if (gSensorPollingPaused) {
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }
    
    // ========================================================================
    // Main Task Logic
    // ========================================================================
    unsigned long now = millis();
    
    if (now - lastRead >= gSettings.<module>DevicePollMs) {
      lastRead = now;
      
      // Read sensor data
      // Update cache
      // Broadcast to ESP-NOW if needed
    }
    
    // ========================================================================
    // Stack Monitoring (every 60s)
    // ========================================================================
    if (now - lastStackLog > 60000) {
      UBaseType_t watermark = uxTaskGetStackHighWaterMark(nullptr);
      
      // Log initial watermark once (shows actual usage after initialization)
      if (!initWatermarkLogged) {
        INFO_SENSORSF("[<Module>] Initial stack watermark: %u words (%u bytes free)", 
                      watermark, watermark * 4);
        initWatermarkLogged = true;
      }
      
      // Log periodic watermark at DEBUG level
      DEBUG_SENSORSF("[<Module>] Stack watermark: %u words", watermark);
      
      // Store for external monitoring
      g<Module>WatermarkNow = watermark;
      if (watermark < g<Module>WatermarkMin || g<Module>WatermarkMin == 0) {
        g<Module>WatermarkMin = watermark;
      }
      
      lastStackLog = now;
    }
    
    // ========================================================================
    // Task Yield
    // ========================================================================
    vTaskDelay(pdMS_TO_TICKS(10));  // Prevent task starvation
  }
}
```

---

## Resolution Steps

### Step 1: Add Standard Header Comments
**What:** Add comprehensive header documentation to each task function
**Why:** Makes it clear what the task does, its resources, and lifecycle
**Files:** All 7 sensor task files

**Example:**
```cpp
// ============================================================================
// IMU Task - FreeRTOS Task Function
// ============================================================================
// Purpose: Continuously reads BNO055 9-DOF sensor data and updates cache
// Stack: 4096 words (~16KB) | Priority: 1 | Core: Any
// Lifecycle: Created by startIMUSensorInternal(), deleted when imuEnabled=false
// Polling: Every 50-1000ms (configurable) | I2C Clock: 100kHz
//
// Cleanup Strategy:
//   1. Check imuEnabled flag
//   2. Acquire i2cMutex to prevent race conditions
//   3. Delete BNO055 object and clear cache
//   4. Release mutex and delete task
// ============================================================================
```

### Step 2: Standardize Startup Logging
**What:** Add consistent startup logging to all tasks
**Why:** Makes debugging easier, confirms task creation
**Pattern:**
```cpp
INFO_SENSORSF("[<Module>] Task started (handle=%p, stack=%u words)", 
              <module>TaskHandle, <STACK_SIZE>);
```

**Files Needing Updates:**
- i2csensor-vl53l4cx.cpp (ToF) - Add startup logging
- i2csensor-pa1010d.cpp (GPS) - Add startup logging
- i2csensor-seesaw.cpp (Gamepad) - Add startup logging

### Step 3: Standardize Cleanup Code
**What:** Ensure all tasks use mutex-protected cleanup
**Why:** Prevents race conditions during shutdown
**Pattern:**
```cpp
if (!<module>Enabled) {
  INFO_SENSORSF("[<Module>] Task shutdown requested");
  
  if (<module>Connected || <sensor> != nullptr) {
    if (i2cMutex && xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
      <module>Connected = false;
      if (<sensor> != nullptr) {
        delete <sensor>;
        <sensor> = nullptr;
      }
      // Clear cache...
      xSemaphoreGive(i2cMutex);
      INFO_SENSORSF("[<Module>] Cleanup complete");
    } else {
      WARN_SENSORSF("[<Module>] Failed to acquire mutex for cleanup");
    }
  }
  
  <module>TaskHandle = nullptr;
  INFO_SENSORSF("[<Module>] Task deleted");
  vTaskDelete(nullptr);
}
```

**Files Already Good:**
- i2csensor-bno055.cpp (IMU) ✅
- i2csensor-mlx90640.cpp (Thermal) ✅

**Files Needing Updates:**
- i2csensor-vl53l4cx.cpp (ToF) - Add mutex protection
- i2csensor-pa1010d.cpp (GPS) - Add mutex protection
- i2csensor-seesaw.cpp (Gamepad) - Add mutex protection
- i2csensor-apds9960.cpp (APDS) - Add INFO logging
- i2csensor-rda5807.cpp (FM Radio) - Add INFO logging

### Step 4: Standardize Stack Monitoring
**What:** Add consistent stack watermark logging
**Why:** Helps identify stack overflow risks
**Pattern:**
```cpp
if (now - lastStackLog > 60000) {
  UBaseType_t watermark = uxTaskGetStackHighWaterMark(nullptr);
  
  if (!initWatermarkLogged) {
    INFO_SENSORSF("[<Module>] Initial stack watermark: %u words (%u bytes free)", 
                  watermark, watermark * 4);
    initWatermarkLogged = true;
  }
  
  DEBUG_SENSORSF("[<Module>] Stack watermark: %u words", watermark);
  
  g<Module>WatermarkNow = watermark;
  if (watermark < g<Module>WatermarkMin || g<Module>WatermarkMin == 0) {
    g<Module>WatermarkMin = watermark;
  }
  
  lastStackLog = now;
}
```

**Files Already Good:**
- i2csensor-bno055.cpp (IMU) ✅
- i2csensor-mlx90640.cpp (Thermal) ✅

**Files Needing Updates:**
- i2csensor-vl53l4cx.cpp (ToF) - Add stack monitoring
- i2csensor-pa1010d.cpp (GPS) - Add stack monitoring
- i2csensor-seesaw.cpp (Gamepad) - Add stack monitoring
- i2csensor-apds9960.cpp (APDS) - Enhance logging
- i2csensor-rda5807.cpp (FM Radio) - Enhance logging

### Step 5: Add Section Comments
**What:** Add clear section markers within task loop
**Why:** Makes code structure obvious at a glance
**Sections:**
1. Graceful Shutdown Check
2. Polling Pause Check
3. Main Task Logic
4. Stack Monitoring
5. Task Yield

---

## Impact Assessment

**Files to Modify:** 7 sensor task files
**Changes Per File:** 
- Header comments: +15 lines
- Startup logging: +2 lines
- Cleanup improvements: +5-10 lines
- Stack monitoring: +10-15 lines
- Section comments: +5 lines
**Total Per File:** ~40-50 lines (mostly comments and logging)

**Risk:** LOW
- Mostly documentation and logging additions
- Cleanup improvements are defensive (prevent race conditions)
- No changes to core sensor reading logic

**Benefits:**
- Easier debugging (consistent logging)
- Better stack overflow detection
- Safer shutdown (mutex-protected cleanup)
- Clearer code structure (section comments)
- Easier onboarding for new developers

---

## Priority Order

### High Priority (Safety Improvements)
1. **i2csensor-vl53l4cx.cpp** - Add mutex-protected cleanup
2. **i2csensor-pa1010d.cpp** - Add mutex-protected cleanup
3. **i2csensor-seesaw.cpp** - Add mutex-protected cleanup

### Medium Priority (Logging & Monitoring)
4. **i2csensor-vl53l4cx.cpp** - Add startup logging and stack monitoring
5. **i2csensor-pa1010d.cpp** - Add startup logging and stack monitoring
6. **i2csensor-seesaw.cpp** - Add startup logging and stack monitoring

### Low Priority (Documentation & Polish)
7. All files - Add header comments and section markers

---

## Example Transformation

### Before (i2csensor-vl53l4cx.cpp - ToF):
```cpp
void tofTask(void* parameter) {
  unsigned long lastToFRead = 0;
  
  while (true) {
    if (!tofEnabled) {
      tofTaskHandle = nullptr;
      vTaskDelete(nullptr);
    }
    
    // Read sensor...
    
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}
```

### After (i2csensor-vl53l4cx.cpp - ToF):
```cpp
// ============================================================================
// ToF Task - FreeRTOS Task Function
// ============================================================================
// Purpose: Continuously reads VL53L4CX distance sensor and updates cache
// Stack: 3072 words (~12KB) | Priority: 1 | Core: Any
// Lifecycle: Created by startToFSensorInternal(), deleted when tofEnabled=false
// Polling: Every 50-5000ms (configurable) | I2C Clock: 400kHz
//
// Cleanup Strategy:
//   1. Check tofEnabled flag
//   2. Acquire i2cMutex to prevent race conditions
//   3. Delete VL53L4CX object and clear cache
//   4. Release mutex and delete task
// ============================================================================

void tofTask(void* parameter) {
  // Startup logging
  INFO_SENSORSF("[ToF] Task started (handle=%p, stack=3072 words)", tofTaskHandle);
  
  unsigned long lastToFRead = 0;
  unsigned long lastStackLog = 0;
  bool initWatermarkLogged = false;
  
  while (true) {
    // ========================================================================
    // Graceful Shutdown Check (CRITICAL - must be first)
    // ========================================================================
    if (!tofEnabled) {
      INFO_SENSORSF("[ToF] Task shutdown requested");
      
      if (tofConnected || tofSensor != nullptr) {
        if (i2cMutex && xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
          tofConnected = false;
          if (tofSensor != nullptr) {
            delete tofSensor;
            tofSensor = nullptr;
          }
          
          // Clear cache
          if (gTofCache.mutex && xSemaphoreTake(gTofCache.mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            gTofCache.tofDataValid = false;
            gTofCache.tofDistance = 0;
            xSemaphoreGive(gTofCache.mutex);
          }
          
          xSemaphoreGive(i2cMutex);
          INFO_SENSORSF("[ToF] Cleanup complete");
        } else {
          WARN_SENSORSF("[ToF] Failed to acquire mutex for cleanup");
        }
      }
      
      tofTaskHandle = nullptr;
      INFO_SENSORSF("[ToF] Task deleted");
      vTaskDelete(nullptr);
    }
    
    // ========================================================================
    // Polling Pause Check
    // ========================================================================
    if (gSensorPollingPaused) {
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }
    
    // ========================================================================
    // Main Task Logic
    // ========================================================================
    unsigned long now = millis();
    
    if (now - lastToFRead >= gSettings.tofPollingMs) {
      lastToFRead = now;
      // Read sensor...
    }
    
    // ========================================================================
    // Stack Monitoring (every 60s)
    // ========================================================================
    if (now - lastStackLog > 60000) {
      UBaseType_t watermark = uxTaskGetStackHighWaterMark(nullptr);
      
      if (!initWatermarkLogged) {
        INFO_SENSORSF("[ToF] Initial stack watermark: %u words (%u bytes free)", 
                      watermark, watermark * 4);
        initWatermarkLogged = true;
      }
      
      DEBUG_SENSORSF("[ToF] Stack watermark: %u words", watermark);
      
      gTofWatermarkNow = watermark;
      if (watermark < gTofWatermarkMin || gTofWatermarkMin == 0) {
        gTofWatermarkMin = watermark;
      }
      
      lastStackLog = now;
    }
    
    // ========================================================================
    // Task Yield
    // ========================================================================
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}
```

---

## Summary

**Goal:** Standardize all 7 sensor task functions with consistent:
1. Header documentation
2. Startup logging
3. Mutex-protected cleanup
4. Stack monitoring
5. Section comments

**Approach:** Incremental updates, starting with safety-critical cleanup improvements
**Risk:** LOW (mostly additive changes)
**Benefit:** Much easier to debug, maintain, and extend
