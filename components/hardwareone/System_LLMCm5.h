/**
 * System_LLMCm5.h — the CM5 co-processor as an LLM answer source.
 *
 * Direction of initiative
 *   The XIAO is the UART *server*; the CM5 daemon is the client. Firmware
 *   cannot call the host and block for an answer — it can only push an EVT
 *   frame and wait for the daemon to come back with commands. So a generation
 *   is a request/stream/terminate machine, modelled on the power/fan ACK
 *   machine in System_Cm5HostControl and on the shipped `g2evenai replypartid`
 *   streaming path, both of which already work this way on hardware.
 *
 * Wire protocol
 *   Firmware → CM5, one EVT frame (length-delimited, so any byte is safe):
 *     llm_ask <session> <maxTokens> <tempX100> <toppX100> <escaped-prompt>
 *     llm_cancel <session>
 *
 *   CM5 → firmware, authenticated command lines consumed by the intrinsic
 *   below — BEFORE cmd_exec, so streamed chunks never take the command lock
 *   and never enter the durable command audit:
 *     cm5 llm models <gen> <idx> <count> <sizeMB> <name>
 *     cm5 llm ready  <gen> <name>
 *     cm5 llm push   <session> <seq> <escaped-text>
 *     cm5 llm end    <session> <ok|error|stopped> [tokens] [tokPerSecX10]
 *
 * Escaping (BOTH directions, see cm5LlmUnescape)
 *   The inbound line is newline-framed and is trimmed before dispatch, so raw
 *   whitespace at a chunk boundary would be silently eaten — and streamed
 *   deltas carry their inter-word space at exactly that boundary. Rather than
 *   protect only the edges, ALL whitespace is escaped so no amount of trimming
 *   or re-tokenizing can damage a chunk:
 *     \\ → backslash   \n → LF   \r → CR   \t → tab   \s → space
 *   Anything else after a backslash is passed through as itself.
 */

#ifndef SYSTEM_LLM_CM5_H
#define SYSTEM_LLM_CM5_H

#include "System_BuildConfig.h"

#if ENABLE_LLM_BACKEND && ENABLE_LLM_SOURCE_CM5

#include <Arduino.h>
#include "System_LLMTypes.h"
#include "System_LLMBackend.h"

// Catalog capacity. The host may serve more models than this; extra rows are
// dropped with a debug line rather than silently truncating the count.
#define CM5_LLM_MAX_MODELS 8

// CONTRACT — catalog names are single, escape-free tokens of at most
// LLM_MODEL_NAME_LEN-1 (31) characters.
//
// This is load-bearing, not a style preference. `models` and `ready` store the
// name with tokCopy() (which does NOT unescape), while cm5LlmSelectByName()
// escapes the stored name again on the way back out. That round trip is an
// identity ONLY while the name contains no whitespace and no backslash — a name
// like "foo bar" would be stored as the literal "foo\sbar" and then escaped a
// second time to "foo\\sbar" on select. The host guarantees the invariant;
// firmware does not re-validate it, so do not "fix" either side alone.

// A generation with no inbound push and no terminator for this long is
// abandoned and finalized as stopped. Without it, one lost `end` line would
// wedge sStreamingTurnSlot in the chat layer for every surface, forever.
//
// Sized against the HOST's keepalive, not against inference time: the daemon
// sends an empty push every 22.5s while a cold model prefills, so this is
// really "how many keepalives may be lost before the turn is abandoned". At
// 45s that was two — and since the firmware admits at most one line per loop
// lap, losing one keepalive to a busy moment put the second one exactly on the
// boundary. 60s buys a third, which is the difference between a race and a
// margin. Raising this is cheaper than asking the host to push more traffic.
//
// CONTRACT: if the host changes its keepalive interval, this moves with it.
#define CM5_LLM_STALL_MS   60000u

// Wire budget for the ESCAPED prompt inside one EVT frame.
//
// UARTLINK_FRAME_MAX_PAYLOAD is 1024 (System_UartLink.h:104) and the
// "llm_ask <session> <maxTokens> <tempX100> <toppX100> " header costs at most
// ~40 bytes, so the escaped prompt must fit in what is left.
//
// This bounds the ESCAPED form, not the raw one, because escaping can double a
// string (every byte a space or newline). The raw ceiling is therefore between
// half this value (all-whitespace) and this value (whitespace-free) — a range,
// not a number, which is why the check below is on the escaped length.
//
// Previously this was 700 and the check compared against 700*2, which let a
// whitespace-free 1400-char prompt through and would have produced a 1430-byte
// payload for a 1024-byte frame. Caught by the CM5 daemon author reading the
// check rather than the comment.
#define CM5_LLM_MAX_PROMPT_ESCAPED 960

// A select the host accepts but never confirms with `cm5 llm ready`.
//
// The inbound vocabulary is models/ready/push/end and nothing else, so there is
// NO line the host can send to report a failed select — and `end` cannot serve
// as one because it is fenced on a generation session that a select never
// mints. The generation stall timer does not cover this either: it is gated on
// !sDone, and a select leaves sDone true. So without a watchdog, an accepted-
// then-failed select leaves every surface showing LOADING forever, cleared only
// by a human running `llmunload`.
//
// Deliberately far above CM5_LLM_STALL_MS: a cold multi-GB GGUF on a
// llama-server that has to restart is tens of seconds, and this is bounding
// "never", not "slow". Presence loss short-circuits it, so the only case that
// waits the full budget is a host that is still alive and still silent.
#define CM5_LLM_SELECT_TIMEOUT_MS 120000u

enum class Cm5LlmCallbackResult : uint8_t {
  NotCallback,
  Handled,
};

// Recognize and consume `cm5 llm <verb> …` from the authenticated UART session.
// Mirrors cm5HostControlHandleCallbackIntrinsic: once the three-token prefix is
// recognized the line is consumed — including on auth/state errors — so a host
// retry can never fall through to cmd_exec or the durable audit.
// `sessionMayControl` is the role decision captured with `namedSessionEpoch`;
// Guest sessions are rejected here exactly as the ordinary authorizer would.
Cm5LlmCallbackResult cm5LlmHandleCallbackIntrinsic(
    const char* line, uint32_t namedSessionEpoch, bool sessionMayControl,
    char* reply, size_t replySize);

// True if `line` begins with the `cm5 llm ` prefix — lets the UART drain decide
// to route it here without parsing twice.
bool cm5LlmIsCallbackLine(const char* line);

// ---- source implementation, called only by System_LLMBackend --------------
size_t    cm5LlmEnumerate(LlmModelDesc* out, size_t cap);
bool      cm5LlmFindByName(const char* name, LlmModelDesc* out);
bool      cm5LlmSelectByName(const char* name, char* errOut, size_t errCap);
void      cm5LlmUnload();
bool      cm5LlmIsReady();
LLMStatus cm5LlmStatus();

// Milliseconds since a model select was armed, or 0 when none is pending.
//
// Nonzero means the HOST is loading, not that anything is transferring: a
// select restarts llama-server on the CM5, so the wait is a multi-GB GGUF read
// there while the link carries only the model's name. Surfacing it lets a UI
// show elapsed progress against CM5_LLM_SELECT_TIMEOUT_MS — both real numbers —
// instead of a static "Loading" that is indistinguishable from a hang.
uint32_t cm5LlmSelectPendingMs();

// Last MEASURED load percentage (0..100) the host reported for a pending
// select, or 0 when nothing is loading. Ratcheted upward and capped below 100
// by the host: residency reaches the model's size ~2s before llama-server
// answers /health, and nothing observes that tail. Display only — no decision
// anywhere reads this.
uint8_t cm5LlmLoadPercent();
int       cm5LlmStartAsync(const char* prompt, const LLMGenParams& params);
void      cm5LlmStop();
int       cm5LlmSessionId();
int       cm5LlmResultLen();
int       cm5LlmResultChunk(int offset, char* buf, int maxLen);
bool      cm5LlmIsDone();
void      cm5LlmTick();

#endif // ENABLE_LLM_BACKEND && ENABLE_LLM_SOURCE_CM5
#endif // SYSTEM_LLM_CM5_H
