// =============================================================================
// System_TextPager — shared text wrap / sanitize / page-split core
// =============================================================================
// Pure string math with no Arduino, G2, or display dependency — usable from any
// surface. Extracted from G2_Page_Common.h (2026-07-20), where it started life
// as the G2 lens' shared paged-text viewer; the lens-specific chrome
// (G2Paginator prev/next rows) and wire render (g2TextPagerRender) stayed
// G2-side. Three consumer families, each using the half that matches its
// constraint:
//
//   G2 lens      wrap at 48 cols + byte-budget pages (BLE single-fragment cap)
//                — G2_Page_Files / Settings / ESPNow via G2_Page_Common.h
//   OLED         wrap at 20 cols, then line-offset scrolling (screen is the
//                constraint, not transport — pages are the wrong model there)
//   serial CLI   sanitize-only (cols=SIZE_MAX disables wrapping; terminals
//                wrap themselves) + byte-budget pages sized to the output caps
//                (web CLI bridge silently truncates ~4 KB) — cmd_fileview
//
// Usage shape (caller owns body[] + pageOff[]):
//   EXT_RAM_BSS_ATTR static char     gBody[CAP];
//   static uint16_t                  gOff[MAX_PAGES + 1];
//   static TextPager pg = { gBody, gOff, MAX_PAGES, PAGE_BUDGET, 0, 0, false };
//   bool trunc = false;
//   size_t n = textWrapInto(gBody, sizeof(gBody), src, cols,
//                           /*indent=*/0, /*stripCtrl=*/true, &trunc);
//   pg.curPage = 0; pg.truncated = (readCapHit || trunc);
//   textSplitPages(pg, n);

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

struct TextPager {
  const char* body;        // caller's wrapped, NUL-terminated body buffer
  uint16_t*   pageOff;     // caller's offsets table (length maxPages + 1)
  int         maxPages;    // capacity of pageOff (minus the end sentinel)
  size_t      pageBudget;  // max body bytes per page
  int         pageCount;   // set by textSplitPages
  int         curPage;     // current page in [0, pageCount)
  bool        truncated;   // body didn't fully fit (read cap, wrap cap, or page cap)
};

// Append a hard-wrapped copy of `src` to `dst` starting at `pos` (advanced in
// place). Wraps at <= `cols` visible columns, indenting continuation lines by
// `contIndent` spaces; embedded '\n' force a fresh line at indent 0. '\r' is
// dropped (CRLF -> LF), '\t' becomes a space, and when `stripCtrl` other
// control bytes (< 0x20, 0x7F) are dropped. Never overflows `dst`; if `src`
// doesn't fully fit, *outTruncated is set to true (never cleared — safe to OR
// across several appends). `dst` is left NUL-terminated.
//
// Sanitize-only mode: pass cols = (size_t)-1 to disable soft wrapping while
// keeping the CRLF/tab/control handling — what a terminal-bound consumer
// (serial fileview) wants, since terminals wrap themselves.
inline void textWrapAppend(char* dst, size_t dstCap, size_t& pos,
                           const char* src, size_t cols, size_t contIndent,
                           bool stripCtrl, bool* outTruncated) {
  if (!dst || dstCap == 0 || !src) return;
  if (cols < 1) cols = 1;
  if (contIndent > cols - 1) contIndent = cols - 1;  // keep >=1 content column
  size_t col = 0;
  const unsigned char* p = (const unsigned char*)src;
  for (; *p; ++p) {
    unsigned char c = *p;
    if (c == '\r') continue;                       // CRLF -> LF
    if (c == '\n') {                               // hard line break
      if (pos + 1 >= dstCap) break;
      dst[pos++] = '\n';
      col = 0;
      continue;
    }
    if (c == '\t') {
      c = ' ';                                     // tabs render as a space
    } else if (stripCtrl && (c < 0x20 || c == 0x7F)) {
      continue;                                    // drop other control bytes
    }
    if (col >= cols) {                             // soft wrap
      if (pos + 1 >= dstCap) break;
      dst[pos++] = '\n';
      col = 0;
      size_t k = 0;
      for (; k < contIndent && pos + 1 < dstCap; ++k) { dst[pos++] = ' '; col++; }
      if (k < contIndent) break;                   // ran out of room mid-indent
    }
    if (pos + 1 >= dstCap) break;
    dst[pos++] = (char)c;
    col++;
  }
  if (outTruncated && *p != '\0') *outTruncated = true;
  dst[(pos < dstCap) ? pos : (dstCap - 1)] = '\0';
}

// Wrap `src` into `dst` from the start; returns bytes written (excl. NUL) and
// clears then possibly sets *outTruncated. Thin wrapper over textWrapAppend.
inline size_t textWrapInto(char* dst, size_t dstCap, const char* src,
                           size_t cols, size_t contIndent, bool stripCtrl,
                           bool* outTruncated) {
  size_t pos = 0;
  if (outTruncated) *outTruncated = false;
  textWrapAppend(dst, dstCap, pos, src, cols, contIndent, stripCtrl,
                 outTruncated);
  return pos;
}

// Slice pager.body[0, totalLen) into pages of <= pager.pageBudget bytes, broken
// at '\n' boundaries. Fills pager.pageOff[0..pageCount] (a start offset per page
// plus an end sentinel) and pager.pageCount. Sets pager.truncated (never clears)
// when maxPages is hit before the end. Post-wrap lines are << the budget, so the
// pathological "single line longer than a whole page" guard rarely fires — but
// it force-advances by pageBudget so an un-wrapped caller can't spin forever.
// NOTE: uint16_t offsets cap the addressable body at 64 KB — cap reads below
// that before splitting.
inline void textSplitPages(TextPager& p, size_t totalLen) {
  p.pageCount = 0;
  if (!p.body || !p.pageOff || p.maxPages < 1 || p.pageBudget == 0) {
    if (p.pageOff) p.pageOff[0] = 0;
    return;
  }
  size_t i = 0;
  while (i < totalLen && p.pageCount < p.maxPages) {
    p.pageOff[p.pageCount++] = (uint16_t)i;
    size_t pe = i;
    while (pe < totalLen) {
      size_t le = pe;
      while (le < totalLen && p.body[le] != '\n') le++;
      size_t lineLen = (le < totalLen) ? (le - pe + 1) : (le - pe);
      if (pe > i && (pe - i) + lineLen > p.pageBudget) break;
      pe += lineLen;
      if (le >= totalLen) break;
    }
    if (pe == i) {                                 // no progress: force-advance
      pe = i + p.pageBudget;
      if (pe > totalLen) pe = totalLen;
    }
    i = pe;
  }
  p.pageOff[p.pageCount] = (uint16_t)i;
  if (i < totalLen) p.truncated = true;
}

// Advance the current page with wrap-around. `forward` = NEXT, else PREV.
// No-op for single-page views.
inline void textNavPage(TextPager& p, bool forward) {
  if (p.pageCount <= 1) return;
  if (!forward) {
    p.curPage = (p.curPage == 0) ? (p.pageCount - 1) : (p.curPage - 1);
  } else {
    p.curPage = (p.curPage + 1) % p.pageCount;
  }
}
