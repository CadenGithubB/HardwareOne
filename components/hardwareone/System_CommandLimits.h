// System_CommandLimits.h - Shared command input/output size contracts.
//
// Keep this header dependency-light: host tests, transport framing, page
// generators, and the firmware executor all consume the same limits.
#ifndef SYSTEM_COMMANDLIMITS_H
#define SYSTEM_COMMANDLIMITS_H

#include <stddef.h>

// Largest complete command line accepted by the shared executor, excluding
// the trailing NUL stored by ExecReq. Transports may impose a smaller limit,
// but must reject a whole over-limit command rather than truncate it.
static constexpr size_t CMD_INPUT_MAX = 2047;

// Largest command result buffer, including its trailing NUL. A handler result
// whose serialized bytes do not fit is rejected at the executor boundary.
static constexpr size_t CMD_RESULT_MAX = 4096;

// A queued/direct command must be non-empty and fit intact in ExecReq::line.
static constexpr bool commandInputLengthAccepted(size_t length) {
  return length > 0 && length <= CMD_INPUT_MAX;
}

#endif  // SYSTEM_COMMANDLIMITS_H
