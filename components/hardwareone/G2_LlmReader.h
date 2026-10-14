#pragma once

// Full-width reply layout. The worker owns the large buffers; this header only
// writes their bounded contents and small page chrome. TextPager supplies the
// shared state, but its byte-wise wrap and wrap-around navigation cannot retain
// UTF-8 boundaries, six-line pages, or a stable position in a growing reply.
#include "System_TextPager.h"
#include <string.h>

namespace G2LlmReader {
constexpr size_t kColumns = 48;
// The 272px-tall widget fits six body lines plus header/footer (about 28px
// per line). Allow every line to fill, including UTF-8's worst two bytes per
// display cell. The worker sends a fragmented REBUILD, not a single UPDATE;
// the former 155-byte body limit stopped ordinary prose after only three lines.
constexpr size_t kBodyLines = 6;
constexpr size_t kBodyBytes = kBodyLines * (kColumns * 2 + 1);
// Worst-case five-digit page counts, generation/truncation marks and footer.
constexpr size_t kFrameCapacity = kBodyBytes + 50;  // Includes NUL.

// rawCapacity includes the input NUL. Even all-newline input must be pageable;
// a byte-budget-only offsets estimate underallocates for that ordinary case.
constexpr size_t wrappedCapacity(size_t rawCapacity) {
  return rawCapacity + rawCapacity / kColumns + 2;
}
constexpr size_t pageOffsetsCapacity(size_t rawCapacity) {
  return rawCapacity / kBodyLines + 2;
}

// raw[0,rawLen) need not be terminated. Suppress an incomplete trailing UTF-8
// character while streaming; show '?' for malformed/final incomplete input.
// Valid text bytes are unchanged except CR/control removal, TAB -> space and
// inserted line wraps. Three-/four-byte characters conservatively occupy two
// cells, matching the glasses' existing transcription layout.
//
// Caller sets pager.pageOff and maxPages; all other layout fields are rebuilt.
// Keep curPage for a streaming update, or set it to zero for a new reply first.
// Caller buffers must not alias raw. Offsets are uint16_t as in TextPager.
inline size_t rebuild(const char* raw, size_t rawLen, bool done,
                      TextPager& pager, char* wrapped, size_t wrappedCap) {
  const int previousPage = pager.curPage;
  pager.body = wrapped;
  pager.pageBudget = kBodyBytes;
  pager.pageCount = 0;
  pager.curPage = 0;
  pager.truncated = false;
  if (pager.pageOff) pager.pageOff[0] = 0;
  if (!wrapped || !wrappedCap) {
    pager.truncated = rawLen != 0;
    return 0;
  }
  wrapped[0] = '\0';
  if (!raw) rawLen = 0;
  const size_t bodyCap = wrappedCap - 1 < UINT16_MAX ? wrappedCap - 1 : UINT16_MAX;
  size_t pos = 0, column = 0, offset = 0;
  while (offset < rawLen) {
    const unsigned char lead = static_cast<unsigned char>(raw[offset]);
    if (lead == '\r' || (lead < 0x20 && lead != '\n' && lead != '\t') || lead == 0x7f) {
      ++offset;
      continue;
    }
    if (lead == '\n') {
      if (pos == bodyCap) { pager.truncated = true; break; }
      wrapped[pos++] = '\n';
      column = 0;
      ++offset;
      continue;
    }

    size_t bytes = 1, consumed = 1, cells = 1;
    bool replacement = false;
    if (lead >= 0x80) {
      size_t need = lead >= 0xc2 && lead <= 0xdf ? 2 :
                    lead >= 0xe0 && lead <= 0xef ? 3 :
                    lead >= 0xf0 && lead <= 0xf4 ? 4 : 1;
      bool valid = need != 1;
      const size_t available = rawLen - offset < need ? rawLen - offset : need;
      for (size_t i = 1; valid && i < available; ++i) {
        const unsigned char next = static_cast<unsigned char>(raw[offset + i]);
        valid = (next & 0xc0) == 0x80;
        if (i == 1) {
          if (lead == 0xe0 && next < 0xa0) valid = false;
          if (lead == 0xed && next >= 0xa0) valid = false;
          if (lead == 0xf0 && next < 0x90) valid = false;
          if (lead == 0xf4 && next >= 0x90) valid = false;
        }
      }
      if (valid && available < need) {
        if (!done) break;
        consumed = available;
        replacement = true;
      } else if (valid) {
        bytes = consumed = need;
        cells = need >= 3 ? 2 : 1;
      } else {
        replacement = true;
      }
    }
    const bool wrap = column + cells > kColumns;
    if (bytes + (wrap ? 1 : 0) > bodyCap - pos) {
      pager.truncated = true;
      break;
    }
    if (wrap) { wrapped[pos++] = '\n'; column = 0; }
    if (replacement) wrapped[pos] = '?';
    else if (lead == '\t') wrapped[pos] = ' ';
    else memcpy(wrapped + pos, raw + offset, bytes);
    pos += bytes;
    column += cells;
    offset += consumed;
  }
  wrapped[pos] = '\0';

  if (!pager.pageOff || pager.maxPages < 1) {
    if (pos) pager.truncated = true;
    return pos;
  }
  size_t start = 0;
  while (start < pos && pager.pageCount < pager.maxPages) {
    pager.pageOff[pager.pageCount++] = static_cast<uint16_t>(start);
    size_t end = start, lines = 0;
    while (end < pos && lines < kBodyLines) {
      size_t next = end;
      while (next < pos && wrapped[next] != '\n') ++next;
      if (next < pos) ++next;
      // Wrapping above guarantees every line fits individually in this budget.
      if (next - start > kBodyBytes) break;
      end = next;
      ++lines;
    }
    start = end;
  }
  pager.pageOff[pager.pageCount] = static_cast<uint16_t>(start);
  if (start < pos) pager.truncated = true;
  if (pager.pageCount) {
    pager.curPage = previousPage < 0 ? 0 :
        previousPage < pager.pageCount ? previousPage : pager.pageCount - 1;
  }
  return pos;
}

inline void navigate(TextPager& pager, bool forward) {
  if (!pager.pageCount) { pager.curPage = 0; return; }
  if (pager.curPage < 0) pager.curPage = 0;
  if (pager.curPage >= pager.pageCount) pager.curPage = pager.pageCount - 1;
  if (forward) {
    if (pager.curPage + 1 < pager.pageCount) ++pager.curPage;
  } else if (pager.curPage) {
    --pager.curPage;
  }
}

// Returns bytes written, or zero (empty output) if the caller's frame is too
// small. No partial UTF-8 frame is ever returned. "..." marks generation;
// "cut" is only a bounded local-layout failure, not normal page navigation.
inline size_t compose(const TextPager& pager, bool done, char* out, size_t cap) {
  if (!out || !cap) return 0;
  out[0] = '\0';
  const bool havePage = pager.body && pager.pageOff && pager.pageCount > 0 &&
      pager.curPage >= 0 && pager.curPage < pager.pageCount;
  const size_t start = havePage ? pager.pageOff[pager.curPage] : 0;
  const size_t end = havePage ? pager.pageOff[pager.curPage + 1] : 0;
  if (end < start || end - start > kBodyBytes) return 0;
  const char* body = havePage ? pager.body + start : done ? "(empty reply)" : "Waiting for reply...";
  const size_t bodyLen = havePage ? end - start : strlen(body);
  char header[40];
  const int headerLen = snprintf(header, sizeof(header), "Reply [%d/%d]%s%s\n",
      havePage ? pager.curPage + 1 : 1, havePage ? pager.pageCount : 1,
      done ? "" : " ...", pager.truncated ? " cut" : "");
  if (headerLen < 0 || static_cast<size_t>(headerLen) >= sizeof(header)) return 0;
  constexpr char footer[] = "Hold: ask | 2x: back";
  const size_t separator = bodyLen && body[bodyLen - 1] == '\n' ? 0 : 1;
  const size_t total = static_cast<size_t>(headerLen) + bodyLen + separator + sizeof(footer) - 1;
  if (total >= cap || total >= kFrameCapacity) return 0;
  memcpy(out, header, static_cast<size_t>(headerLen));
  memcpy(out + headerLen, body, bodyLen);
  size_t pos = static_cast<size_t>(headerLen) + bodyLen;
  if (separator) out[pos++] = '\n';
  memcpy(out + pos, footer, sizeof(footer));
  return total;
}
}  // namespace G2LlmReader
