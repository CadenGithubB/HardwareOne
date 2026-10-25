#pragma once
#include <cstddef>
#include <cstdint>
// Transcript presentation: a bounded user vocabulary (recognition word plus
// display spelling) and the ordinary formatting applied to recognized text
// before it is shown. Portable: no Arduino, ESP-IDF, filesystem, heap, locking
// or inference dependency, so the host suite (test/host/test_stt_transcript.cpp)
// compiles this exact source. System_STTVocabulary owns the firmware instance,
// its file and its locking; the LM decoder (stt_lm.h CustomWords) receives the
// spoken forms as its hotword list.
//
// Ported from the standalone HardwareOne_Speech_To_Text handoff (21 October
// 2026, hw1_transcript). Raw recognition output is never modified: format()
// writes a separate display copy, and callers keep the raw text for receipts,
// acknowledgements and the saved transcript.
namespace hw1::transcript {
constexpr size_t kMaxWords = 256, kSpokenBytes = 31, kDisplayBytes = 63;
struct Word { char spoken[kSpokenBytes + 1]{}; char display[kDisplayBytes + 1]{}; };
// TSV on disk: DISPLAY<TAB>spoken, or DISPLAY alone for ASCII case-only entries.
// The spoken form is what the decoder emits: 1..31 lowercase a-z with internal
// apostrophes (the decoder alphabet); the display form is 1..63 printable
// ASCII bytes with no whitespace or '|', so one recognized word can never
// become a phrase. kMaxWords matches stt_lm.h kMaxCustomWords.
class Vocabulary {
 public:
  // Adds or replaces the entry for `spoken` (derived from `display` lowercased
  // when null/empty). False with `error` filled on an invalid term or a full
  // table; the table is unchanged then.
  bool put(const char* display, const char* spoken, char* error, size_t capacity);
  bool remove(const char* spoken);
  // The display spelling of a raw word (case-insensitive, whole word), or null.
  const char* display(const char* word, size_t bytes) const;
  size_t count() const { return count_; }
  const Word& at(size_t i) const { return words_[i]; }
  void clear() { count_ = 0; }
 private:
  Word words_[kMaxWords]{};
  size_t count_ = 0;
};
// Display copy of `raw`: whitespace collapsed to single spaces and trimmed,
// each word replaced by its configured display spelling when one exists,
// standalone "i" and its contractions (i'm, i've, i'll, i're, i'd) capitalized,
// and the first word capitalized when `paragraphStart`. No punctuation is
// guessed and no word is invented. Returns false on insufficient space; the
// output is then an empty string, never a plausible-looking truncation, so
// callers fall back to the raw text. `raw` and `output` must not alias.
bool format(const char* raw, const Vocabulary& vocabulary, bool paragraphStart,
            char* output, size_t capacity);
}
