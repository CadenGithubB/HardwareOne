#ifndef SYSTEM_LLM_REPLY_LIMITS_H
#define SYSTEM_LLM_REPLY_LIMITS_H

#include <cstddef>

// Shared retained-reply storage limit, including the terminating NUL. HTTP
// reserves room inside this cap for explicit truncation/transport notices;
// chat must retain the whole published result so every surface can read it.
// Buffers of this size belong in PSRAM, never on a worker/display stack.
constexpr size_t kLlmReplyCapacityBytes = 16u * 1024u;

#endif
