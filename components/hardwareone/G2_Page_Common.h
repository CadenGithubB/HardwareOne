// =============================================================================
// G2 hijack page — shared utilities
// =============================================================================
// Paginator: drives the "<< Prev page" / "Next page >> (cur/total)" chrome
// used by every list-mode hijack page that can outgrow a single screen.
// Each page file owns its own row buffer (different row count + row length
// per page), so the helpers take the buffer as parameters rather than
// reaching into a global. Header-only because the bodies are small enough
// that inlining beats a separate TU.
//
// Usage shape (caller owns rows[] + rowPtrs[]):
//   G2Paginator p = g2PaginatorPrepare(itemCount, itemsPerPage, gMyPage);
//   size_t row = /* write back row + items in [p.startIdx, p.endIdx) */;
//   row = g2PaginatorWriteChrome(p, gMyPage, row, MAX_ROWS,
//                                &rows[0][0], ROW_LEN, rowPtrs);
//   /* dispatcher: compare tap idx against p.prevRow / p.nextRow */
// =============================================================================

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

struct G2Paginator {
  size_t startIdx;     // first source item to render on this page
  size_t endIdx;       // one-past-last source item
  size_t totalPages;   // for "(p/total)" trailer display
  int    prevRow;      // -1 if Prev row not shown; else row index
  int    nextRow;      // -1 if Next row not shown; else row index
};

// Clamp `page` to a valid range and compute the [startIdx, endIdx) slice
// for the current page. prev/next row indices start at -1 — the caller
// fills them in via g2PaginatorWriteChrome after writing item rows.
inline G2Paginator g2PaginatorPrepare(size_t itemCount, size_t itemsPerPage,
                                       size_t& page) {
  G2Paginator p;
  p.totalPages = (itemCount == 0)
                   ? 1
                   : (itemCount + itemsPerPage - 1) / itemsPerPage;
  if (page >= p.totalPages) page = p.totalPages - 1;
  p.startIdx = page * itemsPerPage;
  size_t tentativeEnd = p.startIdx + itemsPerPage;
  p.endIdx = (tentativeEnd < itemCount) ? tentativeEnd : itemCount;
  p.prevRow = -1;
  p.nextRow = -1;
  return p;
}

// Append "<< Prev page" / "Next page >> (cur/total)" rows after the
// caller's items. Returns new row count and stores chrome row indices
// back into `p` so the dispatcher can identify taps without re-deriving
// layout. `rowsFlat` points at row 0 (the rows buffer flattened to a
// single char* — `&rows[0][0]` for a `char[N][LEN]` buffer).
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstringop-overflow"
inline size_t g2PaginatorWriteChrome(G2Paginator& p, size_t curPage,
                                      size_t row, size_t maxRow,
                                      char* rowsFlat, size_t rowLen,
                                      const char** rowPtrs) {
  if (rowLen == 0 || !rowsFlat) return row;  // Guard: invalid buffer
  if (curPage > 0 && row < maxRow) {
    char* dst = rowsFlat + row * rowLen;
    snprintf(dst, rowLen, "<< Prev page");
    rowPtrs[row] = dst;
    p.prevRow = (int)row;
    row++;
  }
  if (curPage + 1 < p.totalPages && row < maxRow) {
    char* dst = rowsFlat + row * rowLen;
    snprintf(dst, rowLen, "Next page >> (%u/%u)",
             (unsigned)(curPage + 1), (unsigned)p.totalPages);
    rowPtrs[row] = dst;
    p.nextRow = (int)row;
    row++;
  }
  return row;
}
#pragma GCC diagnostic pop

// =============================================================================
// G2TextPager — shared paged-text viewer (Files / Settings / ESPNow chat)
// =============================================================================
// Three hijack surfaces previously hand-rolled the same "wrap a blob, cut it
// into single-fragment pages, drive tap/scroll paging" logic three different
// ways: Files + Settings each kept an array of up to 24/16 Arduino Strings and
// HARD-CUT any line at the page budget; ESPNow chat used a flat PSRAM buffer +
// offset table and word-wrapped. The chat model is strictly better, so it wins:
// everyone now stores one flat, NUL-terminated body buffer (caller-owned, sized
// to taste) plus an offsets table, and shares the ops below.
//
// The wrap step is what makes long lines legible — a 120-char CSV row or a long
// JSON string value flows across several lens lines instead of being clipped at
// the page budget. Pure string math lives here (no G2 / Arduino dependency); the
// one piece that touches the wire API (assembling chrome + calling
// g2ShowTextPage) is g2TextPagerRender in G2_Glasses.h.
//
// Usage shape (caller owns body[] + pageOff[]):
//   EXT_RAM_BSS_ATTR static char     gBody[CAP];
//   static uint16_t                  gOff[MAX_PAGES + 1];
//   static G2TextPager pg = { gBody, gOff, MAX_PAGES, PAGE_BUDGET, 0, 0, false };
//   bool trunc = false;
//   size_t n = g2TextWrapInto(gBody, sizeof(gBody), src, G2_TEXT_DEFAULT_COLS,
//                             /*indent=*/0, /*stripCtrl=*/true, &trunc);
//   pg.curPage = 0; pg.truncated = (readCapHit || trunc);
//   g2TextSplitPages(pg, n);
//   g2TextPagerRender(pg, ...);            // from G2_Glasses.h
// =============================================================================

// Columns that fit G2_GEOM_LARGE at the lens' default (near-monospace) font.
// Matches the value ESPNow chat proved on hardware.
static constexpr size_t G2_TEXT_DEFAULT_COLS = 48;

struct G2TextPager {
  const char* body;        // caller's wrapped, NUL-terminated body buffer
  uint16_t*   pageOff;     // caller's offsets table (length maxPages + 1)
  int         maxPages;    // capacity of pageOff (minus the end sentinel)
  size_t      pageBudget;  // max body bytes per page
  int         pageCount;   // set by g2TextSplitPages
  int         curPage;     // current page in [0, pageCount)
  bool        truncated;   // body didn't fully fit (read cap, wrap cap, or page cap)
};

// Append a hard-wrapped copy of `src` to `dst` starting at `pos` (advanced in
// place). Wraps at <= `cols` visible columns, indenting continuation lines by
// `contIndent` spaces; embedded '\n' force a fresh line at indent 0. '\r' is
// dropped (CRLF -> LF), '\t' becomes a space, and when `stripCtrl` other
// control bytes (< 0x20, 0x7F) are dropped. Never overflows `dst`; if `src`
// doesn't fully fit, *outTruncated is set to true (never cleared — safe to OR
// across several appends, as chat does). `dst` is left NUL-terminated.
inline void g2TextWrapAppend(char* dst, size_t dstCap, size_t& pos,
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
      c = ' ';                                     // tabs are stripped by the lens
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
// clears then possibly sets *outTruncated. Thin wrapper over g2TextWrapAppend.
inline size_t g2TextWrapInto(char* dst, size_t dstCap, const char* src,
                             size_t cols, size_t contIndent, bool stripCtrl,
                             bool* outTruncated) {
  size_t pos = 0;
  if (outTruncated) *outTruncated = false;
  g2TextWrapAppend(dst, dstCap, pos, src, cols, contIndent, stripCtrl,
                   outTruncated);
  return pos;
}

// Slice pager.body[0, totalLen) into pages of <= pager.pageBudget bytes, broken
// at '\n' boundaries. Fills pager.pageOff[0..pageCount] (a start offset per page
// plus an end sentinel) and pager.pageCount. Sets pager.truncated (never clears)
// when maxPages is hit before the end. Post-wrap lines are << the budget, so the
// pathological "single line longer than a whole page" guard rarely fires — but
// it force-advances by pageBudget so an un-wrapped caller can't spin forever.
inline void g2TextSplitPages(G2TextPager& p, size_t totalLen) {
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

// Advance the current page with wrap-around. `forward` = NEXT (tap / scroll-
// down), else PREV (scroll-up). No-op for single-page views.
inline void g2TextNavPage(G2TextPager& p, bool forward) {
  if (p.pageCount <= 1) return;
  if (!forward) {
    p.curPage = (p.curPage == 0) ? (p.pageCount - 1) : (p.curPage - 1);
  } else {
    p.curPage = (p.curPage + 1) % p.pageCount;
  }
}
