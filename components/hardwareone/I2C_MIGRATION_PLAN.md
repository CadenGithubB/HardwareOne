# I2C System Unified Architecture - Migration Plan

## Current Status
- ✅ Created System_I2C_Device.h/cpp (device abstraction)
- ✅ Created System_I2C_Manager.h/cpp (singleton manager)
- ✅ Created System_I2C_Legacy.h (compatibility wrappers)
- ⚠️ System_I2C.h partially updated (includes new headers)
- ❌ System_I2C.cpp still has old scattered implementation

## Architecture Overview

### Old (Scattered):
```
- 7 different transaction template functions
- gI2CDeviceHealth[] array (global)
- gI2CBusMetrics struct (global)
- gI2CClockStack[] array (global)
- sensorStartQueue[] array (global)
- gSensorPollingPaused flag (global)
- Multiple mutexes: i2cMutex, i2cHealthMutex, queueMutex
```

### New (Unified):
```
I2CDeviceManager (singleton)
├── I2CDevice devices[8]
│   ├── address, name, clockHz, timeoutMs
│   ├── Health (errors, degraded, adaptive timeout)
│   └── transaction() method
├── I2CBusMetrics busMetrics
├── clockStack[8]
├── sensorQueue[8]
├── busMutex (recursive)
├── managerMutex
└── queueMutex
```

## Migration Steps

### Phase 1: Core Infrastructure (IN PROGRESS)
1. ✅ Implement I2CDevice class
2. ✅ Implement I2CDeviceManager singleton
3. ✅ Create legacy compatibility wrappers
4. ⏳ Update System_I2C.cpp to initialize manager
5. ⏳ Keep old globals temporarily for compatibility

### Phase 2: Sensor Registration
1. Update initI2CBuses() to call mgr->initBuses()
2. Auto-register devices from i2cSensors[] database on first use
3. Migrate sensor tasks to use legacy wrappers (no code change needed)

### Phase 3: Command System
1. Update cmd_i2chealth() to query manager->getDevice()
2. Update cmd_i2cmetrics() to use manager->getMetrics()
3. Update cmd_discover() to call manager->discoverDevices()

### Phase 4: Cleanup
1. Remove old globals (gI2CDeviceHealth[], gI2CBusMetrics, etc.)
2. Remove old template functions from System_I2C.h
3. Remove System_I2C_Legacy.h (all sensors migrated to direct API)

## Key Integration Points

### Files That Call I2C Functions:
- i2csensor-*.cpp (7 files) - Use legacy wrappers initially
- oled_display.cpp - Uses i2cDeviceTransactionVoid
- System_I2C.cpp - Discovery, health check, commands
- WebPage_Sensors.cpp - Queries sensor status

### Files That Check gSensorPollingPaused:
- i2csensor-mlx90640.cpp
- i2csensor-vl53l4cx.cpp
- i2csensor-bno055.cpp
- i2csensor-seesaw.cpp
- i2csensor-pa1010d.cpp
- i2csensor-apds9960.cpp
- i2csensor-rda5807.cpp

**Solution**: Keep gSensorPollingPaused as extern, sync with manager->isPollingPaused()

## Next Actions
1. Complete System_I2C.cpp migration
2. Add manager initialization to hardwareone_sketch.cpp setup()
3. Test compilation
4. Verify runtime behavior
