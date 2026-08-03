#include "System_Lz4.h"

#include <string.h>

#include "System_MemUtil.h"

// ─────────────────────────────────────────────────────────────────────
// LZ4 block encoder.
//
// This is the standard "fast" single-hash-table encoder: hash 4 bytes at
// the cursor, look up the last position with the same hash, and if it
// really matches within 64 KB, emit a match instead of literals. No
// match chains, no lazy matching — one candidate per position. That
// costs a few percent of ratio against the reference implementation and
// buys a much smaller, auditable encoder.
//
// Format recap, because the token packing is easy to get subtly wrong:
// each sequence is [token][literal-length ext][literals][offset LE16]
// [match-length ext]. The token's high nibble is the literal count and
// its low nibble is (match length - 4); a nibble of 15 means "read
// extension bytes", each 255 meaning "add 255 and keep reading".
//
// Two invariants the format requires of any encoder, both enforced by
// where the search loop stops:
//   - the last 5 bytes of a block are always literals
//   - the last match must start at least 12 bytes before the end
// ─────────────────────────────────────────────────────────────────────

#define LZ4_MINMATCH     4
#define LZ4_LASTLITERALS 5
#define LZ4_MFLIMIT      12
#define LZ4_HASHLOG      12
#define LZ4_HASHSIZE     (1 << LZ4_HASHLOG)
#define LZ4_MAXOFFSET    65535

size_t lz4BlockBound(size_t inLen) {
  return inLen + (inLen / 255) + 16;
}

size_t lz4FrameBound(size_t inLen) {
  //  4 magic + 2 descriptor + 8 content size + 1 header checksum
  // + 4 block size prefix + 4 end mark = 23
  return lz4BlockBound(inLen) + 23;
}

static inline uint32_t lz4Read32(const uint8_t* p) {
  uint32_t v;
  memcpy(&v, p, sizeof(v));
  return v;  // only ever compared for equality / hashed — endianness is moot
}

static inline uint32_t lz4Hash(uint32_t seq) {
  return (seq * 2654435761U) >> (32 - LZ4_HASHLOG);
}

// Write a length remainder as LZ4 extension bytes (255, 255, …, tail).
static bool lz4WriteLenExt(uint8_t** op, const uint8_t* oend, size_t len) {
  while (len >= 255) {
    if (*op >= oend) return false;
    *(*op)++ = 255;
    len -= 255;
  }
  if (*op >= oend) return false;
  *(*op)++ = (uint8_t)len;
  return true;
}

// Emit one literals+match sequence. The token byte is reserved first and
// backfilled last, because both nibbles are only known after their
// extension bytes have been written.
static bool lz4EmitSequence(uint8_t** op, const uint8_t* oend,
                            const uint8_t* lits, size_t litLen,
                            uint16_t offset, size_t mlCode) {
  if (*op >= oend) return false;
  uint8_t* const token = (*op)++;
  uint8_t t;

  if (litLen >= 15) {
    t = 15 << 4;
    if (!lz4WriteLenExt(op, oend, litLen - 15)) return false;
  } else {
    t = (uint8_t)(litLen << 4);
  }

  if ((size_t)(oend - *op) < litLen) return false;
  memcpy(*op, lits, litLen);
  *op += litLen;

  if ((size_t)(oend - *op) < 2) return false;
  (*op)[0] = (uint8_t)(offset & 0xFF);
  (*op)[1] = (uint8_t)(offset >> 8);
  *op += 2;

  if (mlCode >= 15) {
    t |= 15;
    if (!lz4WriteLenExt(op, oend, mlCode - 15)) return false;
  } else {
    t |= (uint8_t)mlCode;
  }

  *token = t;
  return true;
}

// Trailing literals — same shape as a sequence but with no offset and no
// match nibble, which is how a decoder knows the block has ended.
static bool lz4EmitLastLiterals(uint8_t** op, const uint8_t* oend,
                                const uint8_t* lits, size_t litLen) {
  if (*op >= oend) return false;
  uint8_t* const token = (*op)++;
  uint8_t t;

  if (litLen >= 15) {
    t = 15 << 4;
    if (!lz4WriteLenExt(op, oend, litLen - 15)) return false;
  } else {
    t = (uint8_t)(litLen << 4);
  }

  if ((size_t)(oend - *op) < litLen) return false;
  memcpy(*op, lits, litLen);
  *op += litLen;

  *token = t;
  return true;
}

size_t lz4CompressBlock(const uint8_t* src, size_t srcLen,
                        uint8_t* dst, size_t dstCap) {
  if (!src || !dst || srcLen == 0) return 0;
  if (srcLen > LZ4_MAX_INPUT_BYTES) return 0;

  // Positions are stored +1 so that 0 reads as "nothing hashed here yet"
  // and the table needs no sentinel pass beyond the zeroing calloc gives
  // us. uint16 is sufficient because of the input ceiling above.
  uint16_t* table = (uint16_t*)ps_calloc(LZ4_HASHSIZE, sizeof(uint16_t),
                                         AllocPref::PreferPSRAM, "lz4.table");
  if (!table) return 0;

  const uint8_t* ip     = src;
  const uint8_t* anchor = src;
  const uint8_t* const iend = src + srcLen;

  uint8_t*       op   = dst;
  const uint8_t* oend = dst + dstCap;
  bool overflow = false;

  // Inputs shorter than MFLIMIT can't legally contain a match at all —
  // they fall straight through to the trailing-literals emit below. Keep
  // the backwards pointer arithmetic inside this branch too: forming a
  // pointer before `src` is undefined even when it is never dereferenced.
  if (srcLen > (size_t)LZ4_MFLIMIT) {
    const uint8_t* const mflimit = iend - LZ4_MFLIMIT;
    const uint8_t* const matchlimit = iend - LZ4_LASTLITERALS;
    table[lz4Hash(lz4Read32(ip))] = 1;  // position 0, biased
    ip++;

    while (ip < mflimit) {
      const uint32_t h    = lz4Hash(lz4Read32(ip));
      const uint16_t cand = table[h];
      table[h] = (uint16_t)((ip - src) + 1);

      if (cand == 0) { ip++; continue; }

      const uint8_t* const ref = src + (cand - 1);
      if ((size_t)(ip - ref) > LZ4_MAXOFFSET) { ip++; continue; }
      if (lz4Read32(ref) != lz4Read32(ip))    { ip++; continue; }

      // Confirmed 4-byte match — extend it as far as the match limit
      // allows, then emit everything buffered since the last sequence.
      const uint8_t* const mStart = ip;
      ip += LZ4_MINMATCH;
      const uint8_t* r = ref + LZ4_MINMATCH;
      while (ip < matchlimit && *ip == *r) { ip++; r++; }

      if (!lz4EmitSequence(&op, oend, anchor,
                           /*litLen*/ (size_t)(mStart - anchor),
                           /*offset*/ (uint16_t)(mStart - ref),
                           /*mlCode*/ (size_t)(ip - mStart) - LZ4_MINMATCH)) {
        overflow = true;
        break;
      }
      anchor = ip;
    }
  }

  if (!overflow &&
      !lz4EmitLastLiterals(&op, oend, anchor, (size_t)(iend - anchor))) {
    overflow = true;
  }

  free(table);
  return overflow ? 0 : (size_t)(op - dst);
}

// ─────────────────────────────────────────────────────────────────────
// xxHash32 — needed only for the one header-checksum byte in the frame
// format. Kept local rather than exported; nothing else here wants a
// hash, and a second public hash API would just invite drift.
// ─────────────────────────────────────────────────────────────────────

static inline uint32_t xxhRotl(uint32_t v, int r) {
  return (v << r) | (v >> (32 - r));
}

static uint32_t xxh32(const uint8_t* p, size_t len, uint32_t seed) {
  static const uint32_t P1 = 2654435761U, P2 = 2246822519U,
                        P3 = 3266489917U, P4 = 668265263U, P5 = 374761393U;
  const uint8_t* const end = p + len;
  uint32_t h;

  if (len >= 16) {
    const uint8_t* const limit = end - 16;
    uint32_t v1 = seed + P1 + P2, v2 = seed + P2, v3 = seed, v4 = seed - P1;
    do {
      v1 = xxhRotl(v1 + lz4Read32(p) * P2, 13) * P1; p += 4;
      v2 = xxhRotl(v2 + lz4Read32(p) * P2, 13) * P1; p += 4;
      v3 = xxhRotl(v3 + lz4Read32(p) * P2, 13) * P1; p += 4;
      v4 = xxhRotl(v4 + lz4Read32(p) * P2, 13) * P1; p += 4;
    } while (p <= limit);
    h = xxhRotl(v1, 1) + xxhRotl(v2, 7) + xxhRotl(v3, 12) + xxhRotl(v4, 18);
  } else {
    h = seed + P5;
  }

  h += (uint32_t)len;
  while (p + 4 <= end) { h = xxhRotl(h + lz4Read32(p) * P3, 17) * P4; p += 4; }
  while (p < end)      { h = xxhRotl(h + (*p) * P5, 11) * P1;         p++;    }

  h ^= h >> 15; h *= P2;
  h ^= h >> 13; h *= P3;
  h ^= h >> 16;
  return h;
}

size_t lz4CompressFrame(const uint8_t* src, size_t srcLen,
                        uint8_t* dst, size_t dstCap,
                        bool includeContentSize) {
  if (!src || !dst || srcLen == 0) return 0;
  if (srcLen > LZ4_MAX_INPUT_BYTES) return 0;
  if (dstCap < 23) return 0;

  uint8_t*       op   = dst;
  const uint8_t* oend = dst + dstCap;

  // Magic number 0x184D2204, little-endian on the wire.
  *op++ = 0x04; *op++ = 0x22; *op++ = 0x4D; *op++ = 0x18;

  // Frame descriptor. FLG: version=01, blocks independent, no block
  // checksum, no content checksum, no dictionary. BD: 64 KB block-size
  // class — we always emit a single block and the input ceiling keeps us
  // under it.
  uint8_t* const descStart = op;
  *op++ = (uint8_t)(0x40 | 0x20 | (includeContentSize ? 0x08 : 0x00));
  *op++ = 0x40;
  if (includeContentSize) {
    uint64_t cs = (uint64_t)srcLen;
    for (int i = 0; i < 8; i++) *op++ = (uint8_t)((cs >> (8 * i)) & 0xFF);
  }
  // Header checksum is the *second* byte of the descriptor's xxHash32.
  // Hashed into a local first: reading `op` and incrementing it in one
  // expression has no sequence point between them.
  const uint8_t hc =
      (uint8_t)((xxh32(descStart, (size_t)(op - descStart), 0) >> 8) & 0xFF);
  *op++ = hc;

  // One data block, preceded by its 4-byte little-endian size. Bit 31 of
  // that size means "the bytes that follow are stored, not compressed" —
  // which is the honest thing to do when compression didn't help.
  uint8_t* const blockSizeAt = op;
  op += 4;

  // Everything left over, minus the 4 bytes the end mark still needs.
  if ((size_t)(oend - op) < 4) return 0;
  const size_t roomForBlock = (size_t)(oend - op) - 4;

  size_t blockLen = lz4CompressBlock(src, srcLen, op, roomForBlock);
  bool stored = false;
  if (blockLen == 0 || blockLen >= srcLen) {
    if (roomForBlock < srcLen) return 0;
    memcpy(op, src, srcLen);
    blockLen = srcLen;
    stored = true;
  }
  op += blockLen;

  const uint32_t sizeField = (uint32_t)blockLen | (stored ? 0x80000000U : 0U);
  blockSizeAt[0] = (uint8_t)( sizeField        & 0xFF);
  blockSizeAt[1] = (uint8_t)((sizeField >>  8) & 0xFF);
  blockSizeAt[2] = (uint8_t)((sizeField >> 16) & 0xFF);
  blockSizeAt[3] = (uint8_t)((sizeField >> 24) & 0xFF);

  // End mark: a zero block size.
  *op++ = 0; *op++ = 0; *op++ = 0; *op++ = 0;

  return (size_t)(op - dst);
}
