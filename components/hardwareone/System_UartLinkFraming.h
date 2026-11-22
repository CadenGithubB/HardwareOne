// System_UartLinkFraming.h - the framed command channel of the UART host link
// (link protocol 3, owner decision 2026-11-21: "Full framing now").
//
// A host frames every command line it writes and the link answers it with a
// header that names the request, carries the executor's verdict and says how
// long the reply is:
//
//   request  ->  @<id> <command line>\n
//   reply    <-  @<id> ok <len>\n<len bytes of body>\n
//                @<id> err <len>\n<len bytes of body>\n
//
// <id> is 1-8 lowercase hex digits chosen by the host and echoed verbatim.
// <len> is the body's byte count in plain decimal (0 for an empty body); the
// body may contain newlines. ok/err is the verdict the reply's producer
// reported (the executor's ok flag for a registry command), never a reading of
// the text. A line without the "@<id> " prefix keeps the unframed behaviour (a
// human at a terminal): the reply text and its newline, nothing more.
// Out-of-band traffic (EVT pushes, voicefetch and live PCM frames) carries no
// id. The login reply advertises the version as " link=3" and `uartlink
// status` reports it. The Pi daemon (HardwareOne_RaspPi_CoProcessor,
// docs/ARCHITECTURE.md "Framed command channel") is the other half of this
// contract; both ship together, so there is no unframed fallback for it.
//
// Pure rules here (no heap, no Arduino, no lock) so the host suite tests the
// production code; System_UartLink.cpp owns the port, the lock and the writes.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// The framed protocol version the login reply, `uartlink status` and
// `versions` report. 3 since 2026-11-22 (owner: the wire changed, so the
// version goes up); the Pi daemon requires exactly link=3.
constexpr int kUartLinkProtocolVersion = 3;

// Longest request id (hex digits) and the storage one needs with its NUL.
constexpr size_t kUartFrameIdMax = 8;
constexpr size_t kUartFrameIdStorage = kUartFrameIdMax + 1;

// Longest reply header: "@" + 8 id digits + " err " + 10 length digits + "\n".
constexpr size_t kUartFrameHeaderMax = 1 + kUartFrameIdMax + 5 + 10 + 1;

// The length of the "@<id> " prefix that opens `line` (the id's digits plus
// the '@' and the one space), or 0 when the line is not a framed request: no
// '@' first, no digit, a digit outside [0-9a-f] (uppercase included), more
// than kUartFrameIdMax digits, or anything but one space after them. On
// success the id is copied, NUL-terminated, into idOut (kUartFrameIdStorage
// bytes); on failure idOut is left empty. The command line is what follows
// the prefix, byte for byte.
inline size_t uartFramedRequestPrefix(const char* line, size_t len,
                                      char* idOut) {
  if (idOut) idOut[0] = '\0';
  if (!line || len < 3 || line[0] != '@') return 0;
  size_t digits = 0;
  while (1 + digits < len) {
    const char c = line[1 + digits];
    const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    if (!hex) break;
    if (++digits > kUartFrameIdMax) return 0;
  }
  if (digits == 0 || 1 + digits >= len || line[1 + digits] != ' ') return 0;
  if (idOut) {
    memcpy(idOut, line + 1, digits);
    idOut[digits] = '\0';
  }
  return digits + 2;
}

// Writes the reply header "@<id> ok|err <len>\n" into dst. Returns its length,
// or 0 when id is not a valid request id or dst cannot hold the header (and
// its NUL, which is not part of the wire bytes).
inline size_t uartFrameHeader(char* dst, size_t cap, const char* id, bool ok,
                              size_t bodyLen) {
  if (!dst || cap == 0) return 0;
  dst[0] = '\0';
  if (!id) return 0;
  const size_t idLen = strlen(id);
  if (idLen == 0 || idLen > kUartFrameIdMax) return 0;
  for (size_t i = 0; i < idLen; ++i) {
    const char c = id[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return 0;
  }
  const int n = snprintf(dst, cap, "@%s %s %lu\n", id, ok ? "ok" : "err",
                         static_cast<unsigned long>(bodyLen));
  if (n <= 0 || static_cast<size_t>(n) >= cap) {
    dst[0] = '\0';
    return 0;
  }
  return static_cast<size_t>(n);
}

// One whole reply line for the link's small fixed-buffer replies (login,
// logout, whoami, the control-plane intrinsics, refusals): with a request id,
// "@<id> ok|err <len>\n" + body + "\n"; without one (id null or empty), the
// unframed body + "\n". A body that does not fit is cut to fit, and <len>
// counts what was kept, so the header never promises bytes that do not
// follow. Returns the bytes to write (dst is NUL-terminated after them), or 0
// when dst cannot hold even an empty reply or the id is invalid.
inline size_t uartFrameComposeLine(char* dst, size_t cap, const char* id,
                                   bool ok, const char* body) {
  if (!dst || cap == 0) return 0;
  dst[0] = '\0';
  if (!body) body = "";
  const bool framed = id && id[0];
  size_t bodyLen = strlen(body);
  if (!framed) {
    if (cap < 2) return 0;
    if (bodyLen > cap - 2) bodyLen = cap - 2;
    memcpy(dst, body, bodyLen);
    dst[bodyLen] = '\n';
    dst[bodyLen + 1] = '\0';
    return bodyLen + 1;
  }
  // The header's own length depends on the body length it announces: size it
  // for the full body first, then for the cut body if the full one does not
  // fit (a shorter length never needs more digits).
  char header[kUartFrameHeaderMax + 1];
  size_t headerLen = uartFrameHeader(header, sizeof(header), id, ok, bodyLen);
  if (headerLen == 0) return 0;
  if (headerLen + bodyLen + 2 > cap) {
    if (headerLen + 2 > cap) return 0;
    bodyLen = cap - headerLen - 2;
    headerLen = uartFrameHeader(header, sizeof(header), id, ok, bodyLen);
    if (headerLen == 0 || headerLen + bodyLen + 2 > cap) return 0;
  }
  memcpy(dst, header, headerLen);
  memcpy(dst + headerLen, body, bodyLen);
  dst[headerLen + bodyLen] = '\n';
  dst[headerLen + bodyLen + 1] = '\0';
  return headerLen + bodyLen + 1;
}
