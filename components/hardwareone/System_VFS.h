#ifndef SYSTEM_VFS_H
#define SYSTEM_VFS_H

#include <Arduino.h>
#include <FS.h>

namespace VFS {

enum StorageType {
  INTERNAL = 0,
  SDCARD = 1,
  AUTO = 2
};

bool init();

bool isLittleFSReady();
bool isSDAvailable();

// SD card management
bool remountSD();
bool unmountSD();
bool formatSD();

StorageType getStorageType(const String& path);
String normalize(const String& path);
String stripSdPrefix(const String& path);

bool exists(const String& path);
File open(const String& path, const char* mode = FILE_READ, bool create = false);
bool mkdir(const String& path);
bool remove(const String& path);
bool rename(const String& pathFrom, const String& pathTo);
bool rmdir(const String& path);

bool getStats(StorageType type, uint64_t& totalBytes, uint64_t& usedBytes, uint64_t& freeBytes);

}  // namespace VFS

// SD card command registry (for gCommandModules in System_Utils.cpp)
struct CommandEntry;  // Forward declaration
extern const CommandEntry sdCommands[];
extern const size_t sdCommandsCount;

#endif // SYSTEM_VFS_H
