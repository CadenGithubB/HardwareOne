// System_LLMUtf8.h — serving-edge byte hygiene for offset-addressed poll endpoints.
//
// Deliberately DEPENDENCY-FREE: no Arduino, no ESP-IDF, no project headers. That
// is what lets test/host/ compile and fuzz the REAL functions rather than a copy
// of them — the same rule the other host suites in this repo state
// (updater/test/host/CMakeLists.txt: "the REAL source, not a host
// reimplementation of it"). Keep it that way; adding an include here silently
// deletes the test coverage.
//
// Included by System_LLMChat.h, which is what the firmware actually uses.

#ifndef SYSTEM_LLM_UTF8_H
#define SYSTEM_LLM_UTF8_H

// ---------------------------------------------------------------------------
// Serving-edge byte hygiene for the offset-addressed poll endpoints
// (/api/llm/result and `llmresult json <offset>`).
//
// These are deliberately NOT applied inside chatReadTurn / chatReadStream /
// chatReadFinished. Those readers keep their exact "returns min(avail,
// maxLen-1)" contract, which G2_Glasses.cpp uses as an end-of-data test; making
// them return "at most" would arm that trap. Trim at the serving call site
// instead — which also reaches cmd_llm_result, whose backend bypasses this
// module entirely.
//
// ALWAYS re-terminate the buffer after trimming. ArduinoJson links a const char*
// and walks it with strlen, so a trim without a re-NUL is a silent no-op.
// ---------------------------------------------------------------------------

// Shorten `n` so buf[0..n) never ends inside a multi-byte UTF-8 sequence.
// Returns 0..n. Malformed input (an all-continuation window, an invalid lead)
// passes through unchanged, so the caller's cursor always advances.
//
// INVARIANT: n >= 4 implies the result is >= 1. The longest sequence is 4 bytes,
// so returning 0 needs the lead byte at index 0 with need > n, i.e. n < 4. That
// is why a 511-byte serving window can never produce an empty serve, and why no
// stall is possible. Verified exhaustively over every byte string of length 0..3
// and 22.6M arbitrary windows of length 4..6.
//
// There is deliberately NO "never return 0" guard. One was proposed and would
// have fired whenever the whole window is a single incomplete character — the
// ordinary slow-streaming case — re-introducing the exact defect this exists to
// prevent (measured: 38,346 split characters leaked per 2M cuts, every one of
// them in a window of 3 bytes or fewer).
static inline int utf8TrimPartialTail(const char* p, int n) {
  if (!p || n <= 0) return 0;
  int i = n - 1, steps = 0;
  // The `i >= 0` bound is load-bearing, not defensive: a one-byte window holding
  // a lone continuation byte walks off the front of the buffer without it, which
  // at offset 0 under-reads the PSRAM turn allocation.
  while (i >= 0 && steps < 3 && ((unsigned char)p[i] & 0xC0) == 0x80) { i--; steps++; }
  if (i < 0) return n;                        // all continuations — pass through
  const unsigned char lead = (unsigned char)p[i];
  int need;
  if      (lead < 0x80)           need = 1;
  else if ((lead & 0xE0) == 0xC0) need = 2;
  else if ((lead & 0xF0) == 0xE0) need = 3;
  else if ((lead & 0xF8) == 0xF0) need = 4;
  else return n;                              // invalid lead — pass through
  return (n - i >= need) ? n : i;
}

// Bytes to skip at the FRONT of a window so it does not START inside a
// multi-byte sequence. Returns 0..min(n,3). The mirror image of
// utf8TrimPartialTail — a slice cut out of the MIDDLE of a buffer needs both.
//
// DELIBERATELY UNLIKE utf8TrimPartialTail, this MAY consume the whole window: a
// window of <= 3 bytes that is pure continuation returns n. That is correct for
// its only caller class — a DISPLAY slice recomputed from an absolute buffer
// every frame, where showing three bytes fewer costs nothing — and it would be
// WRONG at an offset-addressed serving edge, where an empty result stalls the
// client's cursor forever.
//
// So: DO NOT call this from handleLLMResult or cmd_llm_result. They cut only a
// tail, never a head. The two functions here have opposite empty-window rules on
// purpose, and that is the single most confusable thing in this header.
//
// The k < 3 bound is the malformed-input escape hatch, matching the trim: a
// valid sequence carries at most 3 continuation bytes, so a 4th means the data
// is garbage and the byte is passed through rather than eaten.
static inline int utf8AlignHead(const char* p, int n) {
  if (!p || n <= 0) return 0;
  int k = 0;
  while (k < n && k < 3 && ((unsigned char)p[k] & 0xC0) == 0x80) k++;
  return k;
}

// Replace, in place, the bytes ArduinoJson would emit RAW into a JSON string.
// Its escape table covers only " \ \b \f \n \r \t; any other byte below 0x20
// is written literally, which produces INVALID JSON. The page's fetch helper has
// no guard for that, so response.json() rejects, the poll's .catch re-polls the
// SAME offset forever, and the turn hangs. A 0x00 is worse still: the linked
// string serializer stops there while the byte count does not, so the cursor
// would advance past bytes that were never sent.
//
// Substitution is 1:1, so the served byte count is unchanged and strlen(buf)
// equals n afterwards — which is what makes the reported cursor authoritative.
static inline void jsonSanitizeServedBytes(char* buf, int n) {
  for (int k = 0; k < n; k++) {
    const unsigned char c = (unsigned char)buf[k];
    if (c < 0x20 && c != '\t' && c != '\n' && c != '\r') buf[k] = ' ';
  }
}

// ---------------------------------------------------------------------------
// Display fold: rewrite UTF-8 to the ASCII subset a CP437 glyph table can draw.
//
// This is NOT serving-edge hygiene and must never be confused with it. The two
// functions above preserve bytes exactly, because a protocol boundary is on the
// other side. This one DESTROYS information on purpose, because the other side
// is an OLED whose font physically cannot draw the character.
//
// Why folding and not UTF-8-aware line wrapping: the OLED draws through
// Adafruit_GFX with the classic font (glcdfont.c is 1280 bytes = 256 glyphs x 5
// columns, indexed font[c * 5 + i]; _cp437 is false and nothing in this
// firmware ever calls cp437() or setFont()). EVERY byte value draws something,
// so an em dash draws three garbage glyphs whether or not a line break splits
// it. Boundary-aware wrapping would therefore fix nothing and would make lines
// narrower than the screen. Folding first restores the invariant that one byte
// is one glyph, which makes the existing byte-based wrap correct by
// construction.
//
// Control bytes are folded too, and that is not cosmetic: a '\n' reaching
// Adafruit_GFX::write() resets cursor_x and advances cursor_y, drawing the rest
// of the line on top of the next one. Turn text is a raw memcpy of engine
// output with no filtering anywhere, so a newline in an answer is ordinary.
//
// IN-PLACE SAFETY: every replacement is no longer than the bytes it replaces,
// so the write cursor never passes the read cursor (w <= r is preserved at each
// step, and both start at 0). The `if (rl > need)` guard is what enforces that
// for the one tight case ("..." for an ellipsis, 3 bytes for 3), and for a
// sequence truncated by the end of the buffer. Adding a longer replacement
// without that guard would corrupt the buffer.
//
// Deliberately different from OLEDConsoleBuffer::append, which DROPS multi-byte
// sequences outright: dropping turns "café" into "caf" and a word can
// vanish silently. Substituting is louder and shorter.
static inline size_t utf8FoldToAscii(char* s, size_t n) {
  // U+00C0..U+00FF folded to a base letter, so European text stays readable
  // instead of becoming a row of '?'. Index is (second byte - 0x80).
  static const char kLatin1[] =
      "AAAAAAECEEEEIIIIDNOOOOOxOUUUUYPs"
      "aaaaaaeceeeeiiiidnooooo/ouuuuypy";
  size_t r = 0, w = 0;
  while (r < n) {
    const unsigned char c = (unsigned char)s[r];
    if (c >= 0x20 && c < 0x7F) { s[w++] = (char)c; r++; continue; }
    if (c < 0x80)              { s[w++] = ' ';     r++; continue; }  // control -> space
    size_t need = ((c & 0xE0) == 0xC0) ? 2
                : ((c & 0xF0) == 0xE0) ? 3
                : ((c & 0xF8) == 0xF0) ? 4
                                       : 1;        // stray continuation / bad lead
    if (r + need > n) need = n - r;                // truncated at the buffer end
    char one[2] = { '?', '\0' };
    const char* rep = one;
    if (need == 2 && c == 0xC3) {
      const unsigned char lo = (unsigned char)s[r + 1];
      if (lo >= 0x80 && lo <= 0xBF) one[0] = kLatin1[lo - 0x80];
    } else if (need == 3 && c == 0xE2 && (unsigned char)s[r + 1] == 0x80) {
      switch ((unsigned char)s[r + 2]) {
        case 0x93: case 0x94: one[0] = '-';  break;   // en dash, em dash
        case 0x98: case 0x99: one[0] = '\''; break;   // curly single quotes
        case 0x9C: case 0x9D: one[0] = '"';  break;   // curly double quotes
        case 0xA6:            rep    = "..."; break;  // ellipsis
        default:              break;                  // stays '?'
      }
    }
    size_t rl = 0;
    while (rep[rl]) rl++;
    if (rl > need) { s[w++] = '?'; r += need; continue; }  // never grow
    for (size_t k = 0; k < rl; k++) s[w + k] = rep[k];
    w += rl; r += need;
  }
  return w;
}

#endif  // SYSTEM_LLM_UTF8_H
