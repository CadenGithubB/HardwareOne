#include <stdint.h>
#include <stddef.h>
#include <atomic>
struct DebugFlagMask { uint64_t w0, w1, w2, w3; };
#define DEBUG_MSG_SIZE 256
struct DebugMessage {            // as shipped
  unsigned long timestamp;
  DebugFlagMask category;
  uint8_t  routing;
  char text[DEBUG_MSG_SIZE];
};
struct DebugMessageGen {         // candidate: generation after routing
  unsigned long timestamp;
  DebugFlagMask category;
  uint8_t  routing;
  uint32_t oledGeneration;
  char text[DEBUG_MSG_SIZE];
};
struct DebugMessageGenTail {     // candidate: generation after text
  unsigned long timestamp;
  DebugFlagMask category;
  uint8_t  routing;
  char text[DEBUG_MSG_SIZE];
  uint32_t oledGeneration;
};
// Exported constants readable from the object file without running it.
extern "C" const unsigned long kSizes[8] = {
  sizeof(DebugMessage), alignof(DebugMessage),
  sizeof(DebugMessageGen), offsetof(DebugMessageGen, oledGeneration),
  sizeof(DebugMessageGenTail), alignof(DebugFlagMask),
  offsetof(DebugMessage, text), offsetof(DebugMessageGen, text)
};
extern "C" const unsigned char kAtomicLockFree = std::atomic<uint32_t>::is_always_lock_free ? 1 : 0;
