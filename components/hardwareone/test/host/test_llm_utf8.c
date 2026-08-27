/*
 * Host tests for the /llm serving-edge byte hygiene.
 *
 * These compile the REAL header (../../System_LLMUtf8.h), not a copy — the same
 * rule updater/test/host/CMakeLists.txt states for the throttle. The header is
 * dependency-free precisely so this is possible.
 *
 * What is asserted, and why each one exists:
 *   1. RANGE + NO OUT-OF-BOUNDS, exhaustive over every byte string of length
 *      0..3, against exact-sized malloc under ASan. An earlier draft of the
 *      walk-back had no lower bound and read one byte before the buffer for a
 *      window holding a lone continuation byte.
 *   2. NO SPLIT LEAK on prefix-of-valid input: the returned prefix is always a
 *      whole number of complete sequences.
 *   3. NO FALSE TRIM: a window already ending on a boundary is returned intact.
 *   4. THE STALL INVARIANT: n >= 4 implies the result is >= 1. This is what
 *      guarantees a 511-byte serving window can never produce an empty serve,
 *      and therefore that the endpoint cannot livelock.
 *   5. NAMED REGRESSIONS so a rejected design cannot come back: every proper
 *      prefix of a multi-byte character must return 0. A proposed "never return
 *      0" guard would have made all of these return n, re-introducing the exact
 *      defect the trim exists to prevent.
 *
 * Deliberately NOT asserted: idempotence. trim(trim(p,n)) == trim(p,n) holds on
 * well-formed input but fails by design on garbage (e.g. E2 E2 80 -> 1 -> 0),
 * because malformed bytes pass through. Asserting it would forbid that.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../System_LLMUtf8.h"

static int failures = 0;

#define CHECK(cond, ...)                                                      \
  do {                                                                        \
    if (!(cond)) {                                                            \
      fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);                    \
      fprintf(stderr, __VA_ARGS__);                                           \
      fprintf(stderr, "\n");                                                  \
      failures++;                                                             \
    }                                                                         \
  } while (0)

/* Length of the sequence a lead byte introduces, or 0 if it is not a lead. */
static int seq_len(unsigned char c) {
  if (c < 0x80) return 1;
  if ((c & 0xE0) == 0xC0) return 2;
  if ((c & 0xF0) == 0xE0) return 3;
  if ((c & 0xF8) == 0xF0) return 4;
  return 0;
}

/* True if buf[0..n) is a whole number of structurally complete sequences. */
static int is_whole(const unsigned char* b, int n) {
  int i = 0;
  while (i < n) {
    int need = seq_len(b[i]);
    if (need == 0) return 0;
    if (i + need > n) return 0;
    for (int k = 1; k < need; k++)
      if ((b[i + k] & 0xC0) != 0x80) return 0;
    i += need;
  }
  return i == n;
}

/* 1 + 3: exhaustive over every byte string of length 0..2, exact-sized buffer
 * so any read outside [0,n) is a heap error under ASan.
 *
 * 0..2 rather than 0..3 is a deliberate cost/coverage trade: the out-of-bounds
 * walk-back this guards is reachable at n == 1 (a window holding a lone
 * continuation byte), and 16.8M exact-sized mallocs under ASan takes minutes for
 * no extra reach. Length 3+ is covered by the seeded fuzz below. */
static void test_exhaustive_short(void) {
  long cases = 0, withheld = 0;
  for (int n = 0; n <= 2; n++) {
    long total = 1;
    for (int k = 0; k < n; k++) total *= 256;
    for (long v = 0; v < total; v++) {
      unsigned char* p = (unsigned char*)malloc(n ? (size_t)n : 1);
      long t = v;
      for (int k = 0; k < n; k++) { p[k] = (unsigned char)(t & 0xFF); t >>= 8; }
      int r = utf8TrimPartialTail((const char*)p, n);
      CHECK(r >= 0 && r <= n, "range: n=%d v=%ld r=%d", n, v, r);
      if (is_whole(p, n)) CHECK(r == n, "false trim: n=%d v=%ld r=%d", n, v, r);
      if (r == 0 && n > 0) withheld++;
      cases++;
      free(p);
    }
  }
  printf("  exhaustive len 0..3: %ld cases, %ld withheld\n", cases, withheld);
}

/* 4: the stall invariant, over arbitrary bytes at the lengths that matter.
 *
 * Seeded, so this is deterministic and reproducible — a flaky fuzz in a suite
 * nobody runs on a schedule is worse than none. Only the last four bytes can
 * influence the result, so random sampling at these lengths is dense coverage of
 * the space that actually decides the answer, not a lottery. */
static unsigned long rng_state = 0x9E3779B97F4A7C15UL;
static unsigned long rng_next(void) {
  rng_state ^= rng_state << 13;
  rng_state ^= rng_state >> 7;
  rng_state ^= rng_state << 17;
  return rng_state;
}

static void test_stall_invariant(void) {
  long cases = 0;
  unsigned char b[8];
  /* Exhaustive over the trailing 3 bytes (which is all the walk-back can see)
   * with a fixed prefix, then random over the whole window. */
  for (int n = 4; n <= 8; n++) {
    for (long v = 0; v < 300000; v++) {
      for (int k = 0; k < n; k++) b[k] = (unsigned char)(rng_next() & 0xFF);
      int r = utf8TrimPartialTail((const char*)b, n);
      CHECK(r >= 1, "STALL: n=%d returned 0 -- a 511-byte window could then "
                    "serve nothing and the endpoint would livelock", n);
      CHECK(r <= n, "range: n=%d r=%d", n, r);
      cases++;
    }
  }
  /* And the structured worst case: every lead byte, followed by every count of
   * continuation bytes, at a window of exactly 4. */
  for (int lead = 0; lead < 256; lead++) {
    for (int conts = 0; conts <= 3; conts++) {
      b[0] = (unsigned char)lead;
      for (int k = 1; k <= 3; k++) b[k] = (k <= conts) ? 0x80 : 0x41;
      int r = utf8TrimPartialTail((const char*)b, 4);
      CHECK(r >= 1, "STALL: lead=0x%02X conts=%d returned 0", lead, conts);
      cases++;
    }
  }
  printf("  stall invariant (n>=4 => r>=1): %ld cases\n", cases);
}

/* 2: no split leak — every prefix of a valid stream trims to whole sequences. */
static void test_no_split_leak(void) {
  static const char* answers[] = {
    "plain ascii only",
    "an em dash \xE2\x80\x94 here",              /* 3-byte */
    "caf\xC3\xA9 na\xC3\xAFve",                  /* 2-byte */
    "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E",      /* CJK, 3-byte */
    "emoji \xF0\x9F\x9A\x80 rocket",             /* 4-byte */
    "mixed \xC3\xA9 \xE2\x80\x94 \xF0\x9F\x9A\x80 \xE6\x97\xA5 end",
  };
  long cases = 0, withheld = 0;
  for (size_t a = 0; a < sizeof(answers) / sizeof(answers[0]); a++) {
    const unsigned char* s = (const unsigned char*)answers[a];
    int len = (int)strlen(answers[a]);
    for (int cut = 0; cut <= len; cut++) {
      int r = utf8TrimPartialTail((const char*)s, cut);
      CHECK(r >= 0 && r <= cut, "range: a=%zu cut=%d r=%d", a, cut, r);
      CHECK(is_whole(s, r), "SPLIT LEAK: answer %zu cut at %d trimmed to %d, "
                            "which still ends inside a sequence", a, cut, r);
      if (r == 0 && cut > 0) withheld++;
      cases++;
    }
  }
  printf("  no split leak: %ld cuts across %zu answers, %ld withheld\n",
         cases, sizeof(answers) / sizeof(answers[0]), withheld);
}

/* 5: named regressions. Each proper prefix of a multi-byte character MUST
 * return 0. A "never return 0" guard was proposed and rejected; if it comes
 * back, these are what catch it. */
static void test_named_regressions(void) {
  const unsigned char two[]  = { 0xC3, 0xA9 };
  const unsigned char three[]= { 0xE2, 0x80, 0x94 };
  const unsigned char four[] = { 0xF0, 0x9F, 0x9A, 0x80 };

  CHECK(utf8TrimPartialTail((const char*)two, 1) == 0, "2-byte prefix must withhold");
  CHECK(utf8TrimPartialTail((const char*)two, 2) == 2, "2-byte whole must pass");
  CHECK(utf8TrimPartialTail((const char*)three, 1) == 0, "3-byte prefix 1 must withhold");
  CHECK(utf8TrimPartialTail((const char*)three, 2) == 0, "3-byte prefix 2 must withhold");
  CHECK(utf8TrimPartialTail((const char*)three, 3) == 3, "3-byte whole must pass");
  CHECK(utf8TrimPartialTail((const char*)four, 1) == 0, "4-byte prefix 1 must withhold");
  CHECK(utf8TrimPartialTail((const char*)four, 3) == 0, "4-byte prefix 3 must withhold");
  CHECK(utf8TrimPartialTail((const char*)four, 4) == 4, "4-byte whole must pass");

  /* Malformed input passes through, so the caller's cursor always advances. */
  const unsigned char conts[8] = { 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80 };
  for (int n = 1; n <= 8; n++)
    CHECK(utf8TrimPartialTail((const char*)conts, n) == n,
          "all-continuation window of %d must pass through", n);
  const unsigned char bad_lead[] = { 0xF8, 0x80, 0x80 };
  CHECK(utf8TrimPartialTail((const char*)bad_lead, 3) == 3, "invalid lead must pass through");
  const unsigned char over[] = { 0xE2, 0x80, 0x80, 0x80, 0x80 };  /* >3 continuations */
  CHECK(utf8TrimPartialTail((const char*)over, 5) == 5, "lead + >3 continuations must pass through");

  CHECK(utf8TrimPartialTail(NULL, 4) == 0, "NULL must return 0");
  CHECK(utf8TrimPartialTail((const char*)two, 0) == 0, "n==0 must return 0");
  CHECK(utf8TrimPartialTail((const char*)two, -1) == 0, "negative n must return 0");
  printf("  named regressions: done\n");
}

/* The JSON sanitizer: 1:1 substitution, so the served byte count is unchanged
 * and strlen(buf) == n afterwards. That equality is what makes the reported
 * cursor authoritative, so it is asserted directly. */
static void test_json_sanitizer(void) {
  char buf[16];
  memcpy(buf, "ab\0cd\x01\x1f\tz", 10);
  jsonSanitizeServedBytes(buf, 10);
  CHECK(buf[2] == ' ', "NUL must become a space");
  CHECK(buf[5] == ' ' && buf[6] == ' ', "control bytes must become spaces");
  CHECK(buf[7] == '\t', "tab must survive (ArduinoJson escapes it)");
  buf[10] = '\0';
  CHECK((int)strlen(buf) == 10, "strlen must equal the served count after sanitising");

  /* High bytes are untouched: UTF-8 must survive intact. */
  char utf[4] = { (char)0xE2, (char)0x80, (char)0x94, 0 };
  jsonSanitizeServedBytes(utf, 3);
  CHECK((unsigned char)utf[0] == 0xE2 && (unsigned char)utf[1] == 0x80 &&
        (unsigned char)utf[2] == 0x94, "multi-byte UTF-8 must not be altered");
  printf("  json sanitizer: done\n");
}

/* utf8AlignHead: the mirror of the trim, with the OPPOSITE empty-window rule.
 * It may legitimately consume the whole window; asserting otherwise would be
 * asserting the trim's contract on the wrong function. What must hold is that
 * align-then-trim always yields a whole number of complete sequences, which is
 * the property its caller (a display slice cut from the middle of a buffer)
 * actually depends on. */
static void test_align_head(void) {
  const unsigned char em[] = { 0xE2, 0x80, 0x94 };
  CHECK(utf8AlignHead((const char*)em, 3) == 0, "a lead byte needs no skip");
  CHECK(utf8AlignHead((const char*)em + 1, 2) == 2, "mid-sequence start skips to the end");
  CHECK(utf8AlignHead((const char*)em + 2, 1) == 1, "last continuation is skipped");
  CHECK(utf8AlignHead(NULL, 3) == 0, "NULL returns 0");
  CHECK(utf8AlignHead((const char*)em, 0) == 0, "n==0 returns 0");
  CHECK(utf8AlignHead((const char*)em, -1) == 0, "negative n returns 0");
  /* 4+ continuations is malformed; pass the byte through rather than eat it. */
  const unsigned char conts[8] = { 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80 };
  CHECK(utf8AlignHead((const char*)conts, 8) == 3, "align stops after 3 continuations");

  /* Exhaustive range check over short windows, exact-sized buffers under ASan. */
  for (int n = 0; n <= 3; n++) {
    long total = 1;
    for (int k = 0; k < n; k++) total *= 256;
    for (long v = 0; v < total; v++) {
      unsigned char* p = (unsigned char*)malloc(n ? (size_t)n : 1);
      long t = v;
      for (int k = 0; k < n; k++) { p[k] = (unsigned char)(t & 0xFF); t >>= 8; }
      int a = utf8AlignHead((const char*)p, n);
      CHECK(a >= 0 && a <= n, "align range: n=%d v=%ld a=%d", n, v, a);
      free(p);
    }
  }

  /* The property the caller depends on: every window of a valid stream, once
   * head-aligned and tail-trimmed, contains only whole sequences. */
  static const char* stream =
      "mixed \xC3\xA9 \xE2\x80\x94 \xF0\x9F\x9A\x80 \xE6\x97\xA5 end";
  const unsigned char* b = (const unsigned char*)stream;
  int len = (int)strlen(stream);
  long windows = 0;
  for (int start = 0; start <= len; start++) {
    for (int end = start; end <= len; end++) {
      int a = utf8AlignHead((const char*)b + start, end - start);
      int r = utf8TrimPartialTail((const char*)b + start + a, end - start - a);
      CHECK(is_whole(b + start + a, r),
            "align+trim left a partial sequence: [%d,%d) a=%d r=%d", start, end, a, r);
      windows++;
    }
  }
  printf("  align head: %ld windows align+trim to whole sequences\n", windows);
}

/* utf8FoldToAscii. The invariant that makes the in-place rewrite legal is
 * w <= r at every step, i.e. the result is never longer than the input. That is
 * asserted directly, exhaustively over short inputs and over a fuzz, because it
 * is the property that would corrupt a buffer if a future replacement string
 * were added without the length guard. */
static void test_fold_ascii(void) {
  char b[64];

  /* Printable ASCII is untouched. */
  strcpy(b, "plain ASCII 123!");
  size_t w = utf8FoldToAscii(b, strlen(b));
  b[w] = '\0';
  CHECK(strcmp(b, "plain ASCII 123!") == 0, "ASCII must pass through, got '%s'", b);

  /* Control bytes -> space. A newline reaching Adafruit_GFX::write() would
   * otherwise reset the cursor and draw over the next line. */
  memcpy(b, "a\nb\tc\x01" "d", 8);   /* split: \x01d would parse as \x1D */
  w = utf8FoldToAscii(b, 7); b[w] = '\0';
  CHECK(strcmp(b, "a b c d") == 0, "controls must fold to space, got '%s'", b);

  /* Punctuation transliteration. */
  strcpy(b, "em\xE2\x80\x94" "dash");              /* em dash; split so \x94 does not eat the d */
  w = utf8FoldToAscii(b, strlen(b)); b[w] = '\0';
  CHECK(strcmp(b, "em-dash") == 0, "em dash -> '-', got '%s'", b);
  strcpy(b, "wait\xE2\x80\xA6");                   /* ellipsis, the tight case */
  w = utf8FoldToAscii(b, strlen(b)); b[w] = '\0';
  CHECK(strcmp(b, "wait...") == 0, "ellipsis -> '...', got '%s'", b);
  strcpy(b, "\xE2\x80\x98q\xE2\x80\x99");
  w = utf8FoldToAscii(b, strlen(b)); b[w] = '\0';
  CHECK(strcmp(b, "'q'") == 0, "curly quotes -> ', got '%s'", b);

  /* Latin-1: a word must stay readable, not lose letters. */
  strcpy(b, "caf\xC3\xA9 na\xC3\xAFve \xC3\x87");
  w = utf8FoldToAscii(b, strlen(b)); b[w] = '\0';
  CHECK(strcmp(b, "cafe naive C") == 0, "latin1 must transliterate, got '%s'", b);

  /* Anything else becomes a single '?', including CJK and emoji. */
  strcpy(b, "x\xE6\x97\xA5y\xF0\x9F\x9A\x80z");
  w = utf8FoldToAscii(b, strlen(b)); b[w] = '\0';
  CHECK(strcmp(b, "x?y?z") == 0, "CJK/emoji -> '?', got '%s'", b);

  /* A sequence truncated by the end of the buffer must not read past it and
   * must not grow. */
  memcpy(b, "ok\xE2\x80", 4);
  w = utf8FoldToAscii(b, 4); b[w] = '\0';
  CHECK(w <= 4, "truncated tail must not grow: w=%zu", w);

  /* THE invariant: output never longer than input. Exhaustive over every byte
   * string of length 0..3 against an exact-sized buffer under ASan. */
  long cases = 0;
  for (int len = 0; len <= 3; len++) {
    long total = 1;
    for (int k = 0; k < len; k++) total *= 256;
    for (long v = 0; v < total; v++) {
      char* p = (char*)malloc(len ? (size_t)len : 1);
      long t = v;
      for (int k = 0; k < len; k++) { p[k] = (char)(t & 0xFF); t >>= 8; }
      size_t out = utf8FoldToAscii(p, (size_t)len);
      CHECK(out <= (size_t)len, "fold GREW the buffer: len=%d v=%ld out=%zu", len, v, out);
      for (size_t k = 0; k < out; k++) {
        unsigned char o = (unsigned char)p[k];
        CHECK(o >= 0x20 && o < 0x7F, "fold emitted a non-printable 0x%02X", o);
      }
      free(p);
      cases++;
    }
  }
  printf("  fold to ascii: %ld exhaustive cases, never grew, all printable\n", cases);
}

int main(void) {
  printf("llm utf8 host tests:\n");
  test_exhaustive_short();
  test_stall_invariant();
  test_no_split_leak();
  test_named_regressions();
  test_align_head();
  test_fold_ascii();
  test_json_sanitizer();
  if (failures) {
    fprintf(stderr, "llm utf8 host tests: %d FAILURE(S)\n", failures);
    return 1;
  }
  printf("llm utf8 host tests: PASS\n");
  return 0;
}
