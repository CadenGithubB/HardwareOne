#pragma once
#include "System_BuildConfig.h"
#include <cstddef>
#include <cstdint>
#if ENABLE_LOCAL_STT
#include "stt/stt_transcript.h"

// The user vocabulary for local speech-to-text: one editable list where each
// entry is a recognition word (what the decoder emits) and its display
// spelling. The same entry serves both uses: its spoken form joins the language
// model's hotword list (stt_lm.h CustomWords, a decoding preference, never a
// guarantee of recognition), and its display form replaces that word whenever
// recognized text is shown. Raw recognized text is never changed: receipts,
// acknowledgements and the saved transcript keep the decoder's words.
//
// File: "/sd/STT Models/user_vocab.tsv" (the card copy wins, like the model
// and LM), else "/STT Models/user_vocab.tsv"; one entry per line, DISPLAY, or
// DISPLAY<TAB>spoken when the spoken form is not just the lowercase display
// (ESP32<TAB>esp). Limits and alphabets: stt_transcript.h. The list is loaded
// once at boot and on `sttvocab reload`; edits validate the whole new list,
// stage it with the shared guarded file writer (previous file kept until the
// new one is complete; SD keeps a .bak) and only then replace the active list.
// An invalid file or edit leaves the active list unchanged. There is no
// account model: the file is device-wide, so edits are admin commands.
//
// Any task; each call holds a short mutex. No call blocks on audio or inference;
// the decoder takes a complete snapshot between phrases (sttVocabularyHotwords)
// and never waits on an edit in progress.
void sttVocabularyInit();   // boot, after the filesystem (and card) is up
bool sttVocabularyReload(char* error, size_t cap);
// spoken may be null/empty: derived from display (lowercased).
bool sttVocabularyAdd(const char* display, const char* spoken, char* error, size_t cap);
bool sttVocabularyRemove(const char* spoken, char* error, size_t cap);
size_t sttVocabularyCount();
bool sttVocabularyEntry(size_t index, hw1::transcript::Word* out);
// The active file ("" before the first load).
const char* sttVocabularyPath();
// Changes on every accepted load/edit (never 0 after the first load).
uint32_t sttVocabularyGeneration();
// Decoder hotwords: the spoken forms, one per line, written to `out` (NUL
// terminated; *bytes excludes it). Returns the generation the text reflects.
// STT_VOCAB_HOTWORD_BYTES fits every entry. The caller parses it with
// CustomWords::parse between phrases when the generation changed.
constexpr size_t STT_VOCAB_HOTWORD_BYTES =
    hw1::transcript::kMaxWords * (hw1::transcript::kSpokenBytes + 1) + 1;
uint32_t sttVocabularyHotwords(char* out, size_t cap, size_t* bytes);
// Display copy of recognized text (stt_transcript.h format() with the active
// vocabulary). False, with an empty output, when it does not fit or the list
// is busy: show the raw text then.
bool sttFormatDisplay(const char* raw, bool paragraphStart, char* out, size_t cap);
#endif
