/**
 * System_LLM_Tokenizer.cpp - merge-based BPE tokenizer (LLM1 embedded format).
 * Extracted verbatim from System_LLM.cpp (no behavioral change).
 */
#include "System_BuildConfig.h"
#if ENABLE_LLM_SOURCE_ONBOARD

#include "System_LLM_Tokenizer.h"
#include "System_LLM_Internal.h"
#include "System_Debug.h"
#include "System_MemUtil.h"  // ps_calloc / AllocPref
#include <Arduino.h>
#include <LittleFS.h>
#include <climits>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <cstdio>

// ============================================================================
// 9. Tokenizer — merge-based BPE (LLM1 embedded format)
// ============================================================================

// Simple open-addressing hash map for merge lookups
static int mergeLookup(const TokenizerState* t, uint32_t left_id, uint32_t right_id,
                       uint32_t* out_merged, int* out_priority) {
  if (!t->merge_map || t->merge_map_capacity == 0) return 0;
  uint32_t key = (left_id << 16) | (right_id & 0xFFFF);
  uint32_t idx = key % (uint32_t)t->merge_map_capacity;
  for (int probe = 0; probe < t->merge_map_capacity; probe++) {
    MergeLookup* e = &t->merge_map[(idx + probe) % t->merge_map_capacity];
    if (e->key == 0 && e->merged_id == 0 && e->priority == 0) return 0;
    if (e->key == key) {
      *out_merged = e->merged_id;
      *out_priority = e->priority;
      return 1;
    }
  }
  return 0;
}

static void mergeMapInsert(TokenizerState* t, uint32_t left_id, uint32_t right_id,
                           uint32_t merged_id, int priority) {
  uint32_t key = (left_id << 16) | (right_id & 0xFFFF);
  uint32_t idx = key % (uint32_t)t->merge_map_capacity;
  for (int probe = 0; probe < t->merge_map_capacity; probe++) {
    MergeLookup* e = &t->merge_map[(idx + probe) % t->merge_map_capacity];
    if (e->key == 0 && e->merged_id == 0 && e->priority == 0) {
      e->key = key;
      e->merged_id = merged_id;
      e->priority = priority;
      return;
    }
  }
}

// Read exactly n bytes; a short read fails the tokenizer load.
static bool tokReadExact(File& f, void* dst, size_t n) {
  return f.read((uint8_t*)dst, n) == n;
}

// Merge keys pack the left and right ids into 16 bits each (mergeLookup), so
// a larger tokenizer vocabulary would make distinct merges collide.
static constexpr uint32_t kTokenizerMaxVocab = 65536;
// On-disk merge record: left, right, merged (u32 each).
static constexpr size_t kMergeRecordBytes = 12;

// Load tokenizer from embedded blob in an open LLM1 file.
// The file position must already be at the tok_byte_len field: byte 64 for a
// model with no info block, or 64 + info_len when loadInfoBlockFromFile consumed
// an info block first. The caller (loadWeights) guarantees this.
// Every count and id is checked against the blob length and the model's
// vocab_size (gLLM.config, parsed before this); any mismatch fails the load and
// the caller unwinds partial allocations through llmUnload().
bool loadTokenizerFromFile(File& f) {
  TokenizerState* t = &gLLM.tokenizer;

  // Read blob size
  uint32_t tok_byte_len = 0;
  if (!tokReadExact(f, &tok_byte_len, 4)) return false;

  const size_t blobStart = f.position();
  const size_t fileSize = f.size();
  if (blobStart > fileSize || tok_byte_len < 8 || tok_byte_len > fileSize - blobStart) {
    ERROR_LLMF("Bad tokenizer blob length: %u", (unsigned)tok_byte_len);
    return false;
  }
  const size_t blobEnd = blobStart + tok_byte_len;

  // Read tokenizer header
  uint32_t tok_vocab_size = 0, merge_count = 0;
  if (!tokReadExact(f, &tok_vocab_size, 4) || !tokReadExact(f, &merge_count, 4)) return false;

  const uint32_t modelVocab = gLLM.config.vocab_size > 0 ? (uint32_t)gLLM.config.vocab_size : 0;
  if (tok_vocab_size == 0 || tok_vocab_size > kTokenizerMaxVocab || tok_vocab_size > modelVocab) {
    ERROR_LLMF("Bad tokenizer vocab_size: %u (model vocab_size %u, limit %u)",
               (unsigned)tok_vocab_size, (unsigned)modelVocab, (unsigned)kTokenizerMaxVocab);
    return false;
  }
  // Each vocab entry needs at least its length byte.
  if (tok_vocab_size > blobEnd - f.position()) {
    ERROR_LLMF("Tokenizer vocab_size %u exceeds the blob", (unsigned)tok_vocab_size);
    return false;
  }

  // The vocab pointer array (4 x V: 9-13 KB for the shipped models) lives in
  // PSRAM with the strings it points to. The cost is one PSRAM pointer read
  // per decoded token beside the matmuls; the gain is that much internal heap
  // left to the task stacks and DMA buffers that cannot live anywhere else
  // (memory audit APP2-02).
  t->vocab = (char**)llmPsramAlloc(tok_vocab_size * sizeof(char*), "tok.vocab");
  if (!t->vocab) return false;

  // First pass: calculate total string pool size, staying inside the blob
  const size_t vocabStart = f.position();
  size_t totalStringBytes = 0;
  for (uint32_t i = 0; i < tok_vocab_size; i++) {
    uint8_t byte_len = 0;
    if (f.position() >= blobEnd || !tokReadExact(f, &byte_len, 1) ||
        byte_len > blobEnd - f.position()) {
      ERROR_LLMF("Tokenizer vocab entry %u runs past the blob", (unsigned)i);
      return false;
    }
    totalStringBytes += byte_len + 1; // +1 for null terminator
    if (byte_len > 0 && !f.seek(f.position() + byte_len)) return false;
  }

  // The merge table fills the rest of the blob exactly.
  const size_t mergeBytes = blobEnd - f.position();
  if (mergeBytes % kMergeRecordBytes != 0 || merge_count != mergeBytes / kMergeRecordBytes ||
      merge_count > (uint32_t)(INT_MAX / 2) ||
      (size_t)merge_count > SIZE_MAX / (2 * sizeof(MergeLookup))) {
    ERROR_LLMF("Tokenizer merge_count %u does not match the blob (%u bytes left)",
               (unsigned)merge_count, (unsigned)mergeBytes);
    return false;
  }

  // Allocate string pool
  char* stringPool = (char*)llmPsramAlloc(totalStringBytes, "tok.strings");
  if (!stringPool) return false;
  gLLM.tokenizerData = stringPool;

  // Second pass: read vocab strings
  if (!f.seek(vocabStart)) return false;
  char* poolPtr = stringPool;
  for (uint32_t i = 0; i < tok_vocab_size; i++) {
    uint8_t byte_len = 0;
    if (!tokReadExact(f, &byte_len, 1)) return false;
    t->vocab[i] = poolPtr;
    if (byte_len > 0 && !tokReadExact(f, poolPtr, byte_len)) return false;
    poolPtr[byte_len] = '\0';
    poolPtr += byte_len + 1;
  }

  // Build byte_to_token lookup (single-byte vocab entries)
  // and collect multi-byte tokens for pre-split matching
  memset(t->byte_to_token, -1, sizeof(t->byte_to_token));
  int multiByteCount = 0;
  for (uint32_t i = 0; i < tok_vocab_size; i++) {
    const char* s = t->vocab[i];
    int slen = strlen(s);
    if (slen == 1) {
      t->byte_to_token[(uint8_t)s[0]] = (int)i;
    } else if (slen >= 2 && slen <= 8) {
      // Candidate for pre-split — count first, allocate after
      multiByteCount++;
    }
  }

  // Pre-split table is built AFTER merge table is loaded (see below)
  t->presplit = nullptr;
  t->presplit_count = 0;

  // Read merge table
  t->merges = nullptr;
  t->merge_map = nullptr;
  t->merge_map_capacity = 0;
  if (merge_count > 0) {
    t->merges = (MergeEntry*)llmPsramAlloc(merge_count * sizeof(MergeEntry), "tok.merges");
    if (!t->merges) return false;

    for (uint32_t i = 0; i < merge_count; i++) {
      MergeEntry* m = &t->merges[i];
      if (!tokReadExact(f, &m->left_id, 4) || !tokReadExact(f, &m->right_id, 4) ||
          !tokReadExact(f, &m->merged_id, 4)) {
        return false;
      }
      if (m->left_id >= tok_vocab_size || m->right_id >= tok_vocab_size ||
          m->merged_id >= tok_vocab_size) {
        ERROR_LLMF("Tokenizer merge %u has an id outside vocab_size %u",
                   (unsigned)i, (unsigned)tok_vocab_size);
        return false;
      }
    }

    // Build merge hash map (2x capacity for low collision rate)
    t->merge_map_capacity = (int)(merge_count * 2);
    if (t->merge_map_capacity < 16) t->merge_map_capacity = 16;
    t->merge_map = (MergeLookup*)llmPsramAlloc(
      t->merge_map_capacity * sizeof(MergeLookup), "tok.mergemap");
    if (!t->merge_map) return false;
    memset(t->merge_map, 0, t->merge_map_capacity * sizeof(MergeLookup));

    for (uint32_t i = 0; i < merge_count; i++) {
      mergeMapInsert(t, t->merges[i].left_id, t->merges[i].right_id,
                     t->merges[i].merged_id, (int)i);
    }
  }

  // Build pre-split table: only tokens that BPE merges CANNOT produce.
  // HuggingFace "added tokens" (like Q: and A:) are matched as whole strings
  // before BPE runs. On device we replicate this by pre-splitting only those
  // tokens whose ID never appears as a mergedId in any merge rule — meaning
  // BPE has no way to construct them from component tokens.
  if (multiByteCount > 0 && merge_count > 0) {
    // Build a set of token IDs that BPE merges can produce
    // Use a simple boolean array (vocab is small enough)
    bool* bpeReachable = (bool*)ps_calloc(tok_vocab_size, sizeof(bool),
                                          AllocPref::PreferPSRAM, "tok.reach");
    if (bpeReachable) {
      for (uint32_t i = 0; i < merge_count; i++) {
        int mid = t->merges[i].merged_id;
        if (mid >= 0 && mid < (int)tok_vocab_size) {
          bpeReachable[mid] = true;
        }
      }

      // Count how many multi-byte tokens are NOT reachable by BPE.
      // Length cap 24, NOT 8: added tokens are whole entity names and the old
      // 8-byte cap silently dropped every name longer than 8 chars (Bulbasaur,
      // Charmander, Charizard, ... — 39 of the 151 Pokemon names), so those
      // fragmented on-device while short names (Pikachu) matched whole.
      const int PRESPLIT_MAX_LEN = 24;
      int unreachableCount = 0;
      for (uint32_t i = 0; i < tok_vocab_size; i++) {
        const char* s = t->vocab[i];
        int slen = strlen(s);
        if (slen >= 2 && slen <= PRESPLIT_MAX_LEN && !bpeReachable[i]) {
          // Check all bytes are individually representable
          bool allMapped = true;
          for (int j = 0; j < slen; j++) {
            if (t->byte_to_token[(uint8_t)s[j]] == -1) { allMapped = false; break; }
          }
          if (allMapped) unreachableCount++;
        }
      }

      if (unreachableCount > 0) {
        // Presplit (1-2 KB for the shipped models): PSRAM first, like the vocab.
        t->presplit = (PreSplitToken*)ps_calloc(unreachableCount, sizeof(PreSplitToken),
                                                AllocPref::PreferPSRAM, "tok.presplit");
        if (t->presplit) {
          int idx = 0;
          for (uint32_t i = 0; i < tok_vocab_size; i++) {
            const char* s = t->vocab[i];
            int slen = strlen(s);
            if (slen < 2 || slen > PRESPLIT_MAX_LEN || bpeReachable[i]) continue;
            bool allMapped = true;
            for (int j = 0; j < slen; j++) {
              if (t->byte_to_token[(uint8_t)s[j]] == -1) { allMapped = false; break; }
            }
            if (!allMapped) continue;
            t->presplit[idx].str = s;
            t->presplit[idx].id = (int)i;
            t->presplit[idx].len = slen;
            idx++;
          }
          t->presplit_count = idx;
          // Sort by length descending so longer matches take priority
          for (int a = 0; a < idx - 1; a++) {
            for (int b = a + 1; b < idx; b++) {
              if (t->presplit[b].len > t->presplit[a].len) {
                PreSplitToken tmp = t->presplit[a];
                t->presplit[a] = t->presplit[b];
                t->presplit[b] = tmp;
              }
            }
          }
        }
      }
      free(bpeReachable);
    }
    if (isDebugOutputEnabled(DEBUG_LLM | DEBUG_LLM_TOKENIZER)) {
      DEBUG_LLM_TOKENIZERF("[LLM] Pre-split tokens: %d entries (from %d multi-byte vocab)", t->presplit_count, multiByteCount);
      for (int j = 0; j < t->presplit_count && j < 10; j++) {
        DEBUG_LLM_TOKENIZERF("[LLM]   presplit[%d] id=%d len=%d \"%s\"",
                              j, t->presplit[j].id, t->presplit[j].len, t->presplit[j].str);
      }
    }
  }

  // The blob must have been consumed exactly; the weights start at blobEnd.
  if (f.position() != blobEnd) {
    ERROR_LLMF("Tokenizer blob pos mismatch: at %u, expected %u",
               (unsigned)f.position(), (unsigned)blobEnd);
    return false;
  }

  t->vocab_size = (int)tok_vocab_size;
  t->merge_count = (int)merge_count;
  DEBUG_LLM_TOKENIZERF("[LLM] Tokenizer loaded: vocab_size=%d merges=%d", t->vocab_size, t->merge_count);
  return true;
}

void freeTokenizer() {
  TokenizerState* t = &gLLM.tokenizer;
  // vocab is PSRAM; presplit PSRAM-first (free() reaches either heap)
  llmPsramFree((void**)&t->vocab);
  llmPsramFree((void**)&t->merges);
  llmPsramFree((void**)&t->merge_map);
  if (t->presplit) { free(t->presplit); t->presplit = nullptr; }
  llmPsramFree((void**)&gLLM.tokenizerData);
  memset(t, 0, sizeof(TokenizerState));
}

// Encode a string into tokens using merge-based BPE
int encode(const char* text, int* tokens, int maxTokens) {
  TokenizerState* t = &gLLM.tokenizer;
  int n_tokens = 0;
  const bool tokenizerDebug = isDebugOutputEnabled(DEBUG_LLM | DEBUG_LLM_TOKENIZER);

  DEBUG_LLM_TOKENIZERF("[LLM][encode] Input (%d chars): \"%.*s%s\"",
                        (int)strlen(text),
                        (int)(strlen(text) > 100 ? 100 : strlen(text)), text,
                        strlen(text) > 100 ? "..." : "");
  DEBUG_LLM_TOKENIZERF("[LLM][encode] Presplit table has %d entries", t->presplit_count);

  // Step 1: scan input, matching multi-byte presplit tokens first,
  // then falling back to single-byte token mapping.
  // This mirrors HuggingFace's handling of added/special tokens:
  // they are matched as whole strings before BPE runs.
  const char* c = text;
  int presplit_hits = 0;
  while (*c != '\0' && n_tokens < maxTokens) {
    // Try presplit tokens (longest first)
    bool matched = false;
    for (int p = 0; p < t->presplit_count; p++) {
      if (strncmp(c, t->presplit[p].str, t->presplit[p].len) == 0) {
        tokens[n_tokens++] = t->presplit[p].id;
        DEBUG_LLM_TOKENIZERF("[LLM][encode] PRESPLIT MATCH at pos %d: \"%s\" -> id=%d",
                              (int)(c - text), t->presplit[p].str, t->presplit[p].id);
        c += t->presplit[p].len;
        matched = true;
        if (tokenizerDebug) presplit_hits++;
        break;
      }
    }
    if (matched) continue;

    // Single-byte fallback
    int id = t->byte_to_token[(uint8_t)*c];
    if (id != -1) {
      tokens[n_tokens++] = id;
    } else {
      DEBUG_LLM_TOKENIZERF("[LLM][encode] Unmapped byte 0x%02X '%c' at pos %d — skipped",
                            (uint8_t)*c, (*c >= 32 && *c < 127) ? *c : '?', (int)(c - text));
    }
    c++;
  }

  DEBUG_LLM_TOKENIZERF("[LLM][encode] After step 1 (presplit+bytes): %d tokens, %d presplit hits",
                        n_tokens, presplit_hits);

  // Debug: dump pre-BPE token list
  if (tokenizerDebug) {
    int show = (n_tokens < 30) ? n_tokens : 30;
    for (int i = 0; i < show; i++) {
      const char* piece = (tokens[i] >= 0 && tokens[i] < t->vocab_size) ?
                           t->vocab[tokens[i]] : "?";
      DEBUG_LLM_TOKENIZERF("[LLM][encode]   pre-bpe[%d] = %d \"%s\"", i, tokens[i], piece);
    }
    if (n_tokens > 30) {
      DEBUG_LLM_TOKENIZERF("[LLM][encode]   ... (%d more)", n_tokens - 30);
    }
  }

  if (n_tokens < 2 || t->merge_count == 0) return n_tokens;

  // Step 2: repeatedly apply the highest-priority (lowest index) applicable merge
  int merge_rounds = 0;
  bool changed = true;
  while (changed) {
    changed = false;
    int best_priority = INT_MAX;
    int best_pos = -1;
    uint32_t best_merged = 0;

    for (int i = 0; i < n_tokens - 1; i++) {
      uint32_t merged;
      int priority;
      if (mergeLookup(t, (uint32_t)tokens[i], (uint32_t)tokens[i + 1], &merged, &priority)) {
        if (priority < best_priority) {
          best_priority = priority;
          best_pos = i;
          best_merged = merged;
        }
      }
    }

    if (best_pos >= 0) {
      if (tokenizerDebug && merge_rounds < 20) {
        const char* lpiece = (tokens[best_pos] >= 0 && tokens[best_pos] < t->vocab_size) ?
                              t->vocab[tokens[best_pos]] : "?";
        const char* rpiece = (tokens[best_pos+1] >= 0 && tokens[best_pos+1] < t->vocab_size) ?
                              t->vocab[tokens[best_pos+1]] : "?";
        const char* mpiece = ((int)best_merged >= 0 && (int)best_merged < t->vocab_size) ?
                              t->vocab[best_merged] : "?";
        DEBUG_LLM_TOKENIZERF("[LLM][encode] BPE merge #%d: pos=%d \"%s\"(%d)+\"%s\"(%d) -> \"%s\"(%u) pri=%d",
                              merge_rounds, best_pos, lpiece, tokens[best_pos],
                              rpiece, tokens[best_pos+1], mpiece, best_merged, best_priority);
      }
      tokens[best_pos] = (int)best_merged;
      for (int i = best_pos + 1; i < n_tokens - 1; i++) {
        tokens[i] = tokens[i + 1];
      }
      n_tokens--;
      changed = true;
      if (tokenizerDebug) merge_rounds++;
    }
  }

  DEBUG_LLM_TOKENIZERF("[LLM][encode] After step 2 (BPE): %d tokens, %d merge rounds", n_tokens, merge_rounds);

  // Debug: dump final token list with special token flags
  if (tokenizerDebug) {
    int show = (n_tokens < 30) ? n_tokens : 30;
    for (int i = 0; i < show; i++) {
      const char* piece = (tokens[i] >= 0 && tokens[i] < t->vocab_size) ?
                           t->vocab[tokens[i]] : "?";
      const char* tag = "";
      if (tokens[i] == 3) tag = " <<<< Q: SPECIAL TOKEN";
      else if (tokens[i] == 4) tag = " <<<< A: SPECIAL TOKEN";
      else if (tokens[i] <= 4) tag = " (special)";
      DEBUG_LLM_TOKENIZERF("[LLM][encode]   final[%d] = %d \"%s\"%s", i, tokens[i], piece, tag);
    }
    if (n_tokens > 30) {
      DEBUG_LLM_TOKENIZERF("[LLM][encode]   ... (%d more)", n_tokens - 30);
    }
  }

  return n_tokens;
}

const char* decode(int prev_token, int token) {
  TokenizerState* t = &gLLM.tokenizer;
  if (token < 0 || token >= t->vocab_size) return "";
  const char* piece = t->vocab[token];
  // Handle raw byte tokens like <0x0A>
  if (piece[0] == '<' && piece[1] == '0' && piece[2] == 'x') {
    static char byte_buf[2];
    unsigned int byte_val;
    sscanf(piece + 1, "0x%02x", &byte_val);
    byte_buf[0] = (char)byte_val;
    byte_buf[1] = '\0';
    return byte_buf;
  }
  return piece;
}


#endif // ENABLE_LLM_SOURCE_ONBOARD
