// System_ESPNow_Catchup.h - file catch-up, device side: the CATCHUP_* server
//
// A device that names a master (espnowmasterfingerprint, and the backup)
// serves its whitelisted logs to them over ESP-NOW (opcodes 82-85,
// FILES_IMPL_SPEC.md sections 2 and 6): LIST pages of its ledger
// (System_CatchupLedger.h) and READ windows of up to 8 frames. The system log,
// saved transcripts and GPS or TEXT sensor captures are sealed on send with
// the capture key; health files go out only when sealed at rest. A byte range
// counts as delivered once one named master has stored it (owner decision Q8).
//
// Threads: the receive handlers (espnow_task) check the reader rule and the
// pacing, then hand the request to cmd_exec; cmd_exec reads the ledger and
// the file and builds the frames; catchupServeTick (espnow_task) sends them,
// paced. No new task.
//
// A build without ENABLE_ESPNOW_SEND answers unsupported (3) once per peer
// session, then counts and drops.
#ifndef SYSTEM_ESPNOW_CATCHUP_H
#define SYSTEM_ESPNOW_CATCHUP_H

#include "System_BuildConfig.h"

#if ENABLE_ESPNOW

#include <Arduino.h>
#include <stdint.h>

#include "System_ESPNow_Verdict.h"  // V4Verdict, what a handler returns (1.0 step 5b)
struct V4RxCtx;

// 82 CATCHUP_LIST_REQ and 84 CATCHUP_READ_REQ (espnow_task). The receive table
// has already required a paired sender and a session-encrypted frame of at
// least the row's minLen.
V4Verdict v4hCatchupListReq(const V4RxCtx& ctx);
V4Verdict v4hCatchupReadReq(const V4RxCtx& ctx);

// espnow_task loop (step 9f2e): sends the next frame of a ready window,
// paced; expires the requester lease; frees the slot when the session is gone.
void catchupServeTick(uint32_t nowMs);

#if ENABLE_ESPNOW_SEND
// Commands (module "catchup", admin).
const char* cmd_espnow_catchup(const String& argsInput);
const char* cmd_espnow_catchupserve(const String& argsInput);
#endif

#endif  // ENABLE_ESPNOW

#endif  // SYSTEM_ESPNOW_CATCHUP_H
