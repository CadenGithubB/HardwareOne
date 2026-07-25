#ifndef G2_PET_H
#define G2_PET_H

// =============================================================================
// G2 glasses — "Pet" app (a Tamagotchi-style virtual creature)
// =============================================================================
// Reached from the Apps launcher (Apps -> Pet). Renders like the camera-stream
// / Maps pages: a static action list on the LEFT ("lstPet") and a locally
// generated, animated creature tile on the RIGHT ("imgPet"), pushed frame by
// frame via Cmd=3 (sendImageBmpFragmentsNoCreate) into the already-CREATEd
// image child. The list can't be REBUILT without dropping the image child, so
// the rows are fixed; feedback is the creature itself changing.
//
// This translation unit is deliberately PORTABLE: it holds only the pet
// simulation (stat decay, actions, persistence) and the pixel-art renderer
// that builds the 4-bpp BMP tile. It has NO dependency on the G2 BLE protocol
// or the hijack machinery — the driving worker/entry/dispatch lives in
// G2_Glasses.cpp (which owns the file-static BLE helpers) and calls into here,
// exactly as g2MapPageWorker drives g2RenderCurrentMapBmp.
//
// Transport reality (measured): the lens has no client-side animation and no
// delta path — every frame re-pushes the whole tile over BLE, so a small tile
// is what keeps the framerate up (an 80x80 tile is a single image fragment).
// A 2-frame idle "breathe" at ~2 fps reads exactly like the original 1996
// Tamagotchi; taps fire short scripted reaction bursts so it feels responsive.
// The lens page is a bounded (<=60 s hijack-watchdog) INTERACTION WINDOW. The
// pet's state is persisted to /system/pet.json, but TIME ONLY PASSES WHILE THE
// PAGE IS OPEN — it resumes exactly where you left it and does NOT age or get
// hungry while closed, so nothing happens behind your back.
//
// v1 scope: the core care loop (Feed / Play / Clean / Sleep, four stats with
// real-time decay, poop/hygiene, save/restore, one hand-drawn creature with
// mood-reactive faces). Life stages / evolution / death, sensor hooks, and a
// play mini-game are deliberately deferred.

#include "System_BuildConfig.h"
#include <stddef.h>
#include <stdint.h>

#if ENABLE_BLUETOOTH && ENABLE_G2_GLASSES

// ── Tile geometry ────────────────────────────────────────────────────────────
// The BMP pushed to the lens each frame. Width MUST be even (4-bpp packs
// 2 px/byte). 80x80 4-bpp = 3318 B, one image-layer fragment (< 3800 B cap),
// which keeps the per-frame BLE cost — and thus the framerate — at its best.
// Bump these (keep W even) for a bigger creature at a lower framerate.
static constexpr int PET_TILE_W = 80;
static constexpr int PET_TILE_H = 80;

// Exact 4-bpp BMP byte size for the tile — size the per-frame push buffer with
// this. Rows are 4-byte aligned: stride = ((W*4+31)/32)*4.
size_t g2PetBmpCap(void);

// ── Action list (LEFT pane) ──────────────────────────────────────────────────
// Row 0 is always Back (handled by the worker). Bit index in the worker's
// pending-tap field == row index. Returns a stable pointer array; *count set.
const char* const* g2PetMenuRows(size_t* count);

// ── Lifecycle driven by the lens worker ──────────────────────────────────────
// Load /system/pet.json and resume the pet exactly where it was left. Time
// passes only while the page is open, so this applies NO offline decay/aging.
void g2PetOpen(void);

// Persist the current pet state. Cheap-ish but not free (atomic FS write) —
// call on a meaningful change / page exit, never per animation frame.
void g2PetSave(void);

// Advance one worker iteration: real-time stat decay (rate-gated to once a
// minute) and the animation phase. Call once per render loop.
void g2PetTick(void);

// Handle an action-row tap (idx per g2PetMenuRows; row 0 = Back is handled by
// the worker and never reaches here). Applies the effect, sets the reaction
// animation, and persists.
void g2PetAction(uint32_t rowIdx);

// True while the pet is asleep.
bool g2PetIsAsleep(void);

// A value that changes whenever the action-row set/labels change (sleep<->wake,
// or alive<->dead). The worker re-CREATEs the compound when it flips — the
// firmware can't REBUILD a single list row without dropping the image child.
uint32_t g2PetMenuVersion(void);

// Render the current pet as a top-down 4-bpp grayscale BMP into `out`.
// Returns the byte count written, or 0 on failure / insufficient cap.
size_t g2RenderPetBmp(uint8_t* out, size_t cap);

// One-line status for the CLI text view (`g2pet`) and diagnostics.
void g2PetBuildInfo(char* out, size_t cap);

#else  // stubs when BLE / G2 disabled

static constexpr int PET_TILE_W = 80;
static constexpr int PET_TILE_H = 80;
inline size_t g2PetBmpCap(void) { return 0; }
inline const char* const* g2PetMenuRows(size_t* count) { if (count) *count = 0; return nullptr; }
inline void g2PetOpen(void) {}
inline void g2PetSave(void) {}
inline void g2PetTick(void) {}
inline void g2PetAction(uint32_t /*rowIdx*/) {}
inline bool g2PetIsAsleep(void) { return false; }
inline uint32_t g2PetMenuVersion(void) { return 0; }
inline size_t g2RenderPetBmp(uint8_t* /*out*/, size_t /*cap*/) { return 0; }
inline void g2PetBuildInfo(char* out, size_t cap) { if (out && cap) out[0] = '\0'; }

#endif  // ENABLE_BLUETOOTH && ENABLE_G2_GLASSES

#endif  // G2_PET_H
