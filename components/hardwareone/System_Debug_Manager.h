#pragma once
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

class DebugManager {
private:
    // Private constructor for singleton
    DebugManager();
    
    // Prevent copying
    DebugManager(const DebugManager&) = delete;
    DebugManager& operator=(const DebugManager&) = delete;
    
public:
    // Singleton access
    static DebugManager& getInstance();
    
    // Public interface (compatibility wrapper around existing debug system)
    void setDebugFlags(uint64_t flags);
    uint64_t getDebugFlags() const;

    void setLogLevel(uint8_t level);
    uint8_t getLogLevel() const;

    void setSystemLogEnabled(bool enabled);
    bool isSystemLogEnabled() const;

    void setLogCategoryTags(bool enabled);
    bool getLogCategoryTags() const;
    
    // Initialize the debug system
    bool initialize();
    
    // Queue a debug message
    void queueDebugMessage(uint64_t flag, const char* message);

    QueueHandle_t getDebugQueue() const;
    QueueHandle_t getDebugFreeQueue() const;
    void incrementDebugDropped();
    
    // Get access to the debug buffer (for compatibility)
    char* getDebugBuffer();
    bool ensureDebugBuffer();
    
    // Cleanup
    void shutdown();
};

// Convenience macros for backward compatibility during transition
#define DEBUG_MANAGER DebugManager::getInstance()
#define GET_DEBUG_FLAGS() DEBUG_MANAGER.getDebugFlags()
#define GET_LOG_LEVEL() DEBUG_MANAGER.getLogLevel()
