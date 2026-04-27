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
inline size_t g2PaginatorWriteChrome(G2Paginator& p, size_t curPage,
                                      size_t row, size_t maxRow,
                                      char* rowsFlat, size_t rowLen,
                                      const char** rowPtrs) {
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
