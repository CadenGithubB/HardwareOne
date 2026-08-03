#pragma once

// ─────────────────────────────────────────────────────────────────────
// System_Lz4 — compress-only LZ4 for the G2 image path.
//
// Why we carry our own: ESP-IDF ships no LZ4 component, and we only ever
// need the *encoder*. The glasses decompress; nothing on this device ever
// reads an LZ4 stream back. A decoder would be dead weight, so there
// isn't one — if you find yourself wanting one, add it deliberately
// rather than assuming it's here.
//
// Context: even_hub_sdk 0.0.12 added LZ4 to the image raw-data path and
// stamps `CompressMode = 2` on ImageRawDataUpdate (see
// 2026-07-19-sdk-image-text-playbook.md). Before that, this firmware only
// ever sent `CompressMode = 0` raw BMP bytes.
//
// Production framing (verified on-device 2026-07-31 via Q16d + Even App
// 2.2.7): LZ4 *bare block* (`lz4CompressBlock`), stamped CompressMode=2.
// Matches `package:dart_lz4` / `lz4BlockCompress` on the phone. FRAME_CSIZE
// paints solo CREATE-image (Q32) but blanks on mixed image children.
// Chunkers keep raw when compression does not shrink the payload.
//
// Frame / size-prefix wraps remain as library archaeology (Q32f menu
// retired 2026-07-31); production and Q32 use lz4CompressBlock only.
//
// Input ceiling is 65535 bytes for both entry points. That is not an LZ4
// limit — it's ours, so the match table can hold uint16 positions (8 KB
// instead of 16 KB). It sits comfortably above the G2's own ceiling: the
// largest legal image container is 288×144, which is a 20,854-byte 4-bpp
// BMP file.
// ─────────────────────────────────────────────────────────────────────

#include <stddef.h>
#include <stdint.h>

// Largest input either entry point accepts. See header comment.
#define LZ4_MAX_INPUT_BYTES 65535u

// Worst-case compressed size for `inLen` bytes of input — an
// incompressible input still costs a token byte per 255 literals plus a
// small fixed tail. Size a destination buffer with this and the encoder
// can never fail for want of room.
size_t lz4BlockBound(size_t inLen);

// Same, for the frame format: block bound plus the frame header
// (4 magic + 2 descriptor + up to 8 content size + 1 checksum), the
// 4-byte block size prefix, and the 4-byte end mark.
size_t lz4FrameBound(size_t inLen);

// LZ4 *block* format — a bare sequence stream with no header and no
// length prefix. The decoder has to already know the decompressed size.
// Returns the compressed length, or 0 on bad input / insufficient
// `dstCap` / allocation failure.
size_t lz4CompressBlock(const uint8_t* src, size_t srcLen,
                        uint8_t* dst, size_t dstCap);

// LZ4 *frame* format — the self-describing container (magic number,
// descriptor, one block, end mark). `includeContentSize` adds the
// 8-byte uncompressed-size field to the descriptor, which lets a
// decoder allocate up front; some decoders want it, some ignore it,
// hence the switch.
//
// Emits exactly one block, so `srcLen` must be <= 64 KB (the block-size
// class declared in the descriptor). Returns the total frame length, or
// 0 on failure.
size_t lz4CompressFrame(const uint8_t* src, size_t srcLen,
                        uint8_t* dst, size_t dstCap,
                        bool includeContentSize);
