/**
 * System_LLM_Tokenizer.h - merge-based BPE tokenizer (LLM1 embedded format).
 *
 * Extracted from System_LLM.cpp with no behavioral change. Operates on the
 * gLLM.tokenizer state (see System_LLM_Internal.h).
 */
#pragma once

#include <FS.h>   // File

// Load the tokenizer blob (vocab + merges + presplit) from an open model file
// into gLLM.tokenizer. Returns false on malformed/short data.
bool loadTokenizerFromFile(File& f);

// Free all tokenizer allocations (vocab, merges, merge map, string pool).
void freeTokenizer();

// Encode UTF-8 text into token ids (presplit specials, then BPE). Returns the
// number of tokens written (<= maxTokens).
int encode(const char* text, int* tokens, int maxTokens);

// Decode one token to its piece string, given the previous token (for the
// SentencePiece leading-space convention). Returns a pointer to internal storage.
const char* decode(int prev_token, int token);
