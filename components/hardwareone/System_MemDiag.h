// Bench-only memory-corruption diagnostics (2026-11-07 P4X panic hunt).
//
// Compiled in only when the build environment sets HW1_BENCH_MEMDIAG=1
// (components/hardwareone/CMakeLists.txt). Every call below is an empty
// inline otherwise, so production builds carry nothing.
//
// What it does on a bench build:
//   - finds IDF's private VFS registry (s_vfs[] and s_vfs_upper_bound) in .bss
//     at boot, logs it, and watches it from a 1 ms poller and a CPU store
//     watchpoint on both cores (a stray CPU store panics at the writing
//     instruction);
//   - runs heap_caps_check_integrity_all(true) at named checkpoints and every
//     few seconds, and logs one MEMDIAG line per checkpoint.
#ifndef SYSTEM_MEMDIAG_H
#define SYSTEM_MEMDIAG_H

#ifndef HW1_BENCH_MEMDIAG
#define HW1_BENCH_MEMDIAG 0
#endif

#if HW1_BENCH_MEMDIAG
void memDiagBootInit();                    // end of setup(): locate, baseline, arm, start the poller
void memDiagCheckpoint(const char* where); // heap integrity + VFS registry check, one log line
void memDiagVfsWriteBegin();               // a legitimate VFS register/unregister follows
void memDiagVfsWriteEnd();                 // ...done: re-baseline and re-arm
const char* memDiagCommand(const char* args);  // `memdiag [check|status|disarm|arm]`
#else
inline void memDiagBootInit() {}
inline void memDiagCheckpoint(const char*) {}
inline void memDiagVfsWriteBegin() {}
inline void memDiagVfsWriteEnd() {}
#endif

// RAII bracket for firmware-owned mount/unmount calls.
struct MemDiagVfsWriteScope {
  MemDiagVfsWriteScope() { memDiagVfsWriteBegin(); }
  ~MemDiagVfsWriteScope() { memDiagVfsWriteEnd(); }
  MemDiagVfsWriteScope(const MemDiagVfsWriteScope&) = delete;
  MemDiagVfsWriteScope& operator=(const MemDiagVfsWriteScope&) = delete;
};

#endif
