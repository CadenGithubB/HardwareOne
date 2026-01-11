#ifndef FILESYSTEM_H
#define FILESYSTEM_H

#include <Arduino.h>

// Forward declarations
class String;

// ============================================================================
// Filesystem State
// ============================================================================

// Global filesystem ready flag (defined in filesystem.cpp)
extern bool filesystemReady;

// ============================================================================
// Filesystem Helper Functions
// ============================================================================

/**
 * Build a directory listing as either JSON or human-readable text
 * @param inPath Directory path to list
 * @param out Output string (by reference)
 * @param asJson true for JSON format, false for text
 * @return true on success, false on error
 */
bool buildFilesListing(const String& inPath, String& out, bool asJson);

// Filesystem command registry (for system_utils.cpp)
struct CommandEntry;  // Forward declaration

// Boot sequence management
void loadAndIncrementBootSeq();
extern const CommandEntry filesystemCommands[];
extern const size_t filesystemCommandsCount;

/**
 * Initialize the filesystem (LittleFS)
 * @return true if initialized successfully
 */
bool initFilesystem();

// ============================================================================
// File Permissions and Protection
// ============================================================================

/**
 * File permission flags
 */
enum FilePermission {
  PERM_READ = 0x01,
  PERM_WRITE = 0x02,
  PERM_DELETE = 0x04,
  PERM_EDIT = (PERM_READ | PERM_WRITE),
  PERM_ALL = (PERM_READ | PERM_WRITE | PERM_DELETE)
};

/**
 * Check if a file/folder can be deleted
 * @param path Absolute path to check
 * @return true if deletion is allowed
 */
bool canDelete(const String& path);

/**
 * Check if a file can be edited (written to)
 * @param path Absolute path to check
 * @return true if editing is allowed
 */
bool canEdit(const String& path);

/**
 * Check if a file/folder can be created in the given path
 * @param path Absolute path to check
 * @return true if creation is allowed
 */
bool canCreate(const String& path);

/**
 * Get permission flags for a given path
 * @param path Absolute path to check
 * @return Bitmask of FilePermission flags
 */
uint8_t getPermissions(const String& path);

// ============================================================================
// File I/O Helpers
// ============================================================================

/**
 * Read up to maxBytes from a file into a String
 * @param path File path
 * @param out Output string
 * @param maxBytes Maximum bytes to read
 * @return true on success
 */
bool readTextLimited(const char* path, String& out, size_t maxBytes);

/**
 * Append a line to a file, enforcing a maximum file size
 * @param path File path
 * @param line Line to append
 * @param capBytes Maximum file size in bytes
 * @return true on success
 */
bool appendLineWithCap(const char* path, const String& line, size_t capBytes);

#endif // FILESYSTEM_H
