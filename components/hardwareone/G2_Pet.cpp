#include "G2_Pet.h"

#if ENABLE_BLUETOOTH && ENABLE_G2_GLASSES

#include <Arduino.h>
#include <ArduinoJson.h>
#include <esp_attr.h>
#include <esp_random.h>
#include <time.h>

#include "System_Utils.h"     // readText, writeTextAtomic, everyMs
#include "System_MemUtil.h"   // PSRAM_JSON_DOC
#include "System_Clock.h"     // Clock::epochSeconds / isSynced
#include "System_Debug.h"     // DEBUG_G2F

// =============================================================================
// Pet simulation + pixel-art renderer.  Portable: no BLE / hijack dependencies.
// The lens worker in G2_Glasses.cpp owns the transport and calls in here.
//
// Phase 2 adds a life cycle: Egg -> Baby -> Child -> Adult (care-branched into a
// happy or a grumpy form), sickness + medicine, death + rebirth, an on-demand
// numeric stats readout, and assorted animation polish. The creature body is a
// single hand-drawn 16x16 sprite reused across the growing stages, differentiated
// by scale + drawn-on accessories; Egg and Ghost are their own sprites.
// =============================================================================

#define PET_FILE "/system/pet.json"

// ── Sprite geometry ──────────────────────────────────────────────────────────
static constexpr int PET_SPR    = 16;
static constexpr int PET_BASE_Y = 70;   // creature "feet" baseline in the tile

// ── Life-cycle tuning (real minutes of age; tune freely) ─────────────────────
static constexpr uint32_t HATCH_MIN = 3;      // egg  -> baby
static constexpr uint32_t CHILD_MIN = 20;     // baby -> child
static constexpr uint32_t ADULT_MIN = 90;     // child -> adult
static constexpr uint32_t MAX_AGE_MIN = 4320; // adult old-age death (~3 days)
static constexpr int      CARE_ADULT_THRESHOLD = 8;   // careScore for the happy form
static constexpr uint32_t DEATH_CRITICAL_MIN   = 20;  // sustained neglect -> death

// ── Palette (16-level gray; lens renders these as shades of green) ───────────
static inline int petSym(char c) {
  switch (c) {
    case '#': return 0;    // outline / darkest
    case '=': return 4;
    case '-': return 6;    // closed-lid / soft shade
    case 'o': return 10;   // body base
    case 'O': return 12;   // body light
    case '@': return 14;   // belly / highlight
    case '*': return 15;   // white / shine
    default:  return -1;   // '.' / ' ' → transparent
  }
}

// ── Creature body (one 16x16, shared Baby/Child/Adult) ───────────────────────
// Only the eye row (r7) and mouth row (r9) change with mood; the body is shared
// through the B* macros. Each row is exactly 16 chars.
#define B0  "................"
#define B1  "......####......"
#define B2  ".....#oooo#....."
#define B3  "....#oooooo#...."
#define B4  "...#oooooooo#..."
#define B5  "..#oooooooooo#.."
#define B6  "..#oooooooooo#.."
#define B8  "..#oooooooooo#.."
#define B10 "..#oooooooooo#.."
#define B11 "...#oooooooo#..."
#define B12 "...#oooooooo#..."
#define B13 "....#oooooo#...."
#define B14 ".....######....."
#define B15 "................"

#define EYES_OPEN   "..#oo##oo##oo#.."
#define EYES_SHUT   "..#oo--oo--oo#.."
#define MOUTH_FLAT  "..#oooo##oooo#.."
#define MOUTH_SMILE "..#ooo####ooo#.."
#define MOUTH_FROWN "..#oo#oooo#oo#.."

static const char* const PET_IDLE[PET_SPR] = {
  B0,B1,B2,B3,B4,B5,B6, EYES_OPEN, B8, MOUTH_FLAT,  B10,B11,B12,B13,B14,B15 };
static const char* const PET_BLINK[PET_SPR] = {
  B0,B1,B2,B3,B4,B5,B6, EYES_SHUT, B8, MOUTH_FLAT,  B10,B11,B12,B13,B14,B15 };
static const char* const PET_HAPPY[PET_SPR] = {
  B0,B1,B2,B3,B4,B5,B6, EYES_OPEN, B8, MOUTH_SMILE, B10,B11,B12,B13,B14,B15 };
static const char* const PET_SAD[PET_SPR] = {
  B0,B1,B2,B3,B4,B5,B6, EYES_OPEN, B8, MOUTH_FROWN, B10,B11,B12,B13,B14,B15 };

// ── Egg / Ghost (their own sprites) ──────────────────────────────────────────
static const char* const PET_EGG[PET_SPR] = {
  "................",
  "......####......",
  ".....#OOOO#.....",
  "....#OOOOOO#....",
  "...#OOOOOOOO#...",
  "...#OO@OOOOO#...",
  "..#OOOOOOOOOO#..",
  "..#OOOOOO@OOO#..",
  "..#OOOOOOOOOO#..",
  "..#O@OOOOOOOO#..",
  "..#OOOOOOOOOO#..",
  "...#OOOOOOOO#...",
  "...#OOO@OOOO#...",
  "....#OOOOOO#....",
  ".....######.....",
  "................" };
static const char* const PET_EGG_CRACK[PET_SPR] = {
  "................",
  "......####......",
  ".....#OOOO#.....",
  "....#OOOOOO#....",
  "...#OOOOOOOO#...",
  "...#OO@OOOOO#...",
  "..#OOOO##OOOO#..",
  "..#OO##OO##OO#..",
  "..#O##OOOO##O#..",
  "..#OO##OO##OO#..",
  "..#OOOO##OOOO#..",
  "...#OOOOOOOO#...",
  "...#OOO@OOOO#...",
  "....#OOOOOO#....",
  ".....######.....",
  "................" };
static const char* const PET_GHOST[PET_SPR] = {
  "................",
  "......####......",
  ".....#OOOO#.....",
  "....#OOOOOO#....",
  "...#OOOOOOOO#...",
  "..#OOOOOOOOOO#..",
  "..#OO##OO##OO#..",
  "..#OO##OO##OO#..",
  "..#OOOOOOOOOO#..",
  "..#OOO####OOO#..",
  "..#OOOOOOOOOO#..",
  "..#OOOOOOOOOO#..",
  "..#OOOOOOOOOO#..",
  "..#O#OO#OO#OO#..",
  "..#.##.##.##.#..",
  "................" };

// 5x5 heart, used on feed/play/clean reactions.
static const char* const HEART5[5] = { ".#.#.", "#####", "#####", ".###.", "..#.." };

// ── Tiny 3x5 font for the on-demand stats readout (digits + H/P/E/C) ─────────
static const char* petGlyph(char c) {
  switch (c) {
    case '0': return "###" "#.#" "#.#" "#.#" "###";
    case '1': return ".#." "##." ".#." ".#." "###";
    case '2': return "###" "..#" "###" "#.." "###";
    case '3': return "###" "..#" "###" "..#" "###";
    case '4': return "#.#" "#.#" "###" "..#" "..#";
    case '5': return "###" "#.." "###" "..#" "###";
    case '6': return "###" "#.." "###" "#.#" "###";
    case '7': return "###" "..#" "..#" "..#" "..#";
    case '8': return "###" "#.#" "###" "#.#" "###";
    case '9': return "###" "#.#" "###" "..#" "###";
    case 'H': return "#.#" "#.#" "###" "#.#" "#.#";
    case 'P': return "###" "#.#" "###" "#.." "#..";
    case 'E': return "###" "#.." "###" "#.." "###";
    case 'C': return "###" "#.." "#.." "#.." "###";
    default:  return nullptr;
  }
}

// ── Life stage ───────────────────────────────────────────────────────────────
enum PetStage : uint8_t { ST_EGG = 0, ST_BABY, ST_CHILD, ST_ADULT_A, ST_ADULT_B, ST_DEAD };

// ── Pet state (persisted) ────────────────────────────────────────────────────
static float    sHunger  = 80, sHappy = 80, sEnergy = 80, sHygiene = 80;  // 0..100
static uint32_t sAgeMin  = 0;
static bool     sAsleep  = false;
static bool     sPoop    = false;
static bool     sSick    = false;
static uint8_t  sStage   = ST_EGG;
static int      sCare    = 0;         // care score → adult branch
static uint32_t sCritMin = 0;         // sustained-neglect minutes → death
static uint32_t sBornEpoch = 0;
static uint32_t sLastEpoch = 0;

// ── Volatile view state (not persisted) ──────────────────────────────────────
static uint32_t sAnimPhase   = 0;
static uint32_t sDecayStamp  = 0;
static bool     sLoaded      = false;
static bool     sStatsShown  = false;
static uint32_t sEvolveMs    = 0;     // evolution-burst animation start

enum PetReaction : uint8_t { PR_NONE = 0, PR_EAT, PR_HAPPY, PR_CLEAN, PR_TIRED, PR_MED };
static uint8_t  sReaction    = PR_NONE;
static uint32_t sReactionEnd = 0;

static const int8_t BOB[8]       = { 0, -1, -2, -1, 0, 1, 2, 1 };
static const int8_t BOB_SLEEP[8] = { 0,  0, -1, -1, 0, 0, 1, 1 };
static const int8_t BOB_GHOST[8] = { 0, -1, -2, -3, -2, -1, 0, 1 };

static inline float clampf(float v) { return v < 0 ? 0 : (v > 100 ? 100 : v); }
static inline bool  petAlive(void)  { return sStage != ST_DEAD; }
static inline bool  petHatched(void){ return sStage != ST_EGG && sStage != ST_DEAD; }

static void setReaction(uint8_t r, uint32_t ms) {
  sReaction = r;
  sReactionEnd = ms ? (millis() + ms) : 0;
}
static void addCare(int d) { sCare += d; if (sCare > 60) sCare = 60; if (sCare < -30) sCare = -30; }

// ── Decay (closed form so a multi-hour offline gap is one call) ──────────────
static void petApplyMinutes(float m) {
  if (m <= 0 || !petHatched()) return;
  const float sickX = sSick ? 1.6f : 1.0f;
  if (sAsleep) {
    sEnergy  = clampf(sEnergy  + m * 2.0f);
    sHunger  = clampf(sHunger  - m * 0.3f);
    sHappy   = clampf(sHappy   - m * 0.2f * sickX);
    sHygiene = clampf(sHygiene - m * 0.1f);
  } else {
    sHunger  = clampf(sHunger  - m * 0.8f);
    sEnergy  = clampf(sEnergy  - m * 0.5f * sickX);
    sHappy   = clampf(sHappy   - m * (sPoop ? 1.0f : 0.5f) * sickX);
    sHygiene = clampf(sHygiene - m * (sPoop ? 0.6f : 0.25f));
  }
}

static uint8_t petTargetStage(void) {
  if (sAgeMin < HATCH_MIN) return ST_EGG;
  if (sAgeMin < CHILD_MIN) return ST_BABY;
  if (sAgeMin < ADULT_MIN) return ST_CHILD;
  return (sCare >= CARE_ADULT_THRESHOLD) ? ST_ADULT_A : ST_ADULT_B;
}

// Advance to the next stage if age crossed a threshold. Adult form locks once
// chosen (careScore afterwards no longer flips a grown adult).
static void petCheckEvolution(void) {
  if (sStage == ST_DEAD) return;
  if (sStage == ST_ADULT_A || sStage == ST_ADULT_B) return;
  const uint8_t t = petTargetStage();
  if (t != sStage) {
    sStage = t;
    sEvolveMs = millis();
    setReaction(PR_HAPPY, 1600);
    DEBUG_G2F("[G2-PET] evolved → stage %u (care %d)", (unsigned)sStage, sCare);
  }
}

static void petDie(void) {
  if (sStage == ST_DEAD) return;
  sStage = ST_DEAD;
  sAsleep = false; sSick = false; sPoop = false;
  setReaction(PR_NONE, 0);
  DEBUG_G2F("[G2-PET] the pet has died (age %luh, care %d)",
            (unsigned long)(sAgeMin / 60), sCare);
}

// ── Persistence ──────────────────────────────────────────────────────────────
static void petLoadDefaults(void) {
  sHunger = sHappy = sEnergy = sHygiene = 80;
  sAgeMin = 0; sAsleep = false; sPoop = false; sSick = false;
  sStage = ST_EGG; sCare = 0; sCritMin = 0;
  const uint32_t now = (uint32_t)Clock::epochSeconds();
  sBornEpoch = now; sLastEpoch = now;
}

static void petReadFile(void) {
  String json;
  if (!readText(PET_FILE, json) || json.length() == 0) { petLoadDefaults(); return; }
  PSRAM_JSON_DOC(doc);
  if (deserializeJson(doc, json) != DeserializationError::Ok) { petLoadDefaults(); return; }
  sHunger    = (float)(int)(doc["hunger"]  | 80);
  sHappy     = (float)(int)(doc["happy"]   | 80);
  sEnergy    = (float)(int)(doc["energy"]  | 80);
  sHygiene   = (float)(int)(doc["hygiene"] | 80);
  sAgeMin    = doc["ageMin"]  | 0;
  sAsleep    = doc["asleep"]  | false;
  sPoop      = doc["poop"]    | false;
  sSick      = doc["sick"]    | false;
  sStage     = doc["stage"]   | (int)ST_EGG;
  sCare      = doc["care"]    | 0;
  sCritMin   = doc["crit"]    | 0;
  sBornEpoch = doc["born"]    | 0;
  sLastEpoch = doc["last"]    | 0;
  if (sStage > ST_DEAD) sStage = ST_CHILD;
  if (sBornEpoch == 0) sBornEpoch = (uint32_t)Clock::epochSeconds();
}

void g2PetSave(void) {
  PSRAM_JSON_DOC(doc);
  doc["v"]       = 2;
  doc["hunger"]  = (int)(sHunger  + 0.5f);
  doc["happy"]   = (int)(sHappy   + 0.5f);
  doc["energy"]  = (int)(sEnergy  + 0.5f);
  doc["hygiene"] = (int)(sHygiene + 0.5f);
  doc["ageMin"]  = sAgeMin;
  doc["asleep"]  = sAsleep;
  doc["poop"]    = sPoop;
  doc["sick"]    = sSick;
  doc["stage"]   = sStage;
  doc["care"]    = sCare;
  doc["crit"]    = sCritMin;
  doc["born"]    = sBornEpoch;
  const uint32_t now = (uint32_t)Clock::epochSeconds();
  if (now) sLastEpoch = now;
  doc["last"]    = sLastEpoch;
  String out;
  serializeJson(doc, out);
  if (!writeTextAtomic(PET_FILE, out)) DEBUG_G2F("[G2-PET] save failed (%s)", PET_FILE);
}

void g2PetOpen(void) {
  petReadFile();
  // Time passes ONLY while the Pet page is open — NO offline catch-up. The pet
  // resumes exactly where you left it; nothing decays, ages, or evolves while
  // you're away, so you never miss anything. (A background-progression mode
  // would be a deliberate future opt-in, not the default.)
  if (sAsleep && sEnergy >= 100.0f) sAsleep = false;
  sDecayStamp = millis();
  sReaction = PR_NONE; sReactionEnd = 0;
  sStatsShown = false;
  sLoaded = true;
  DEBUG_G2F("[G2-PET] open: stage %u H%d P%d E%d C%d age%luh%s%s%s",
            (unsigned)sStage, (int)sHunger, (int)sHappy, (int)sEnergy, (int)sHygiene,
            (unsigned long)(sAgeMin / 60), sAsleep ? " sleeping" : "",
            sSick ? " sick" : "", sPoop ? " dirty" : "");
  g2PetSave();
}

// ── Per-loop tick ────────────────────────────────────────────────────────────
void g2PetTick(void) {
  if (!sLoaded) return;
  if (everyMs(&sDecayStamp, 60000)) {           // once per real minute
    petApplyMinutes(1.0f);
    sAgeMin++;
    if (petHatched() && !sAsleep && !sPoop && (esp_random() % 10) == 0) sPoop = true;

    if (petHatched() && !sSick) {               // sickness onset from neglect
      const bool risk = (sHygiene <= 0 || sHunger <= 0 || sPoop);
      if (risk && (int)(esp_random() % 100) < (sPoop ? 8 : 20)) { sSick = true; addCare(-2); }
    }
    if (petHatched() && (sHunger <= 0 || sHappy <= 0 || sHygiene <= 0)) addCare(-1);

    // Death from sustained neglect, or old age.
    const bool dying = petHatched() && !sAsleep &&
                       (sHunger <= 0 || sHygiene <= 0 || sSick);
    sCritMin = dying ? (sCritMin + 1) : 0;
    if (sCritMin >= DEATH_CRITICAL_MIN) petDie();
    if ((sStage == ST_ADULT_A || sStage == ST_ADULT_B) && sAgeMin >= MAX_AGE_MIN) petDie();
  }
  if (sAsleep && sEnergy >= 100.0f) { sAsleep = false; setReaction(PR_HAPPY, 1400); }
  petCheckEvolution();
  sAnimPhase++;
}

// ── Actions (row taps; row 0 = Back handled by the worker) ───────────────────
void g2PetAction(uint32_t idx) {
  if (!sLoaded) return;

  if (sStage == ST_DEAD) {           // dead menu: [1] New Egg
    if (idx == 1) { petLoadDefaults(); sLoaded = true; sDecayStamp = millis();
                    setReaction(PR_HAPPY, 1400); g2PetSave(); }
    return;
  }

  switch (idx) {
    case 1:  // Feed
      sAsleep = false;
      if (sHunger < 40) addCare(1);
      sHunger = clampf(sHunger + 35);
      sHappy  = clampf(sHappy + 3);
      setReaction(PR_EAT, 1600);
      break;
    case 2:  // Play
      sAsleep = false;
      if (sEnergy < 15) { setReaction(PR_TIRED, 1400); }
      else {
        if (sHappy < 60) addCare(1);
        sHappy  = clampf(sHappy + 20);
        sEnergy = clampf(sEnergy - 12);
        sHunger = clampf(sHunger - 3);
        setReaction(PR_HAPPY, 1600);
      }
      break;
    case 3:  // Clean
      if (sPoop) { sPoop = false; sHygiene = clampf(sHygiene + 45); addCare(1); }
      else       {                sHygiene = clampf(sHygiene + 15); }
      setReaction(PR_CLEAN, 1500);
      break;
    case 4:  // Medicine
      if (sSick) { sSick = false; sHygiene = clampf(sHygiene + 10); sHappy = clampf(sHappy + 5); addCare(1); }
      else       { sHappy = clampf(sHappy + 3); }
      setReaction(PR_MED, 1500);
      break;
    case 5:  // Sleep / Wake toggle
      sAsleep = !sAsleep;
      setReaction(PR_NONE, 0);
      break;
    case 6:  // Stats readout toggle (view-only, no save)
      sStatsShown = !sStatsShown;
      return;
    default:
      return;
  }
  g2PetSave();
}

bool g2PetIsAsleep(void) { return sAsleep; }

// Menu changes (→ compound re-CREATE) only on dead/alive and sleep/wake flips.
uint32_t g2PetMenuVersion(void) {
  return (uint32_t)((sStage == ST_DEAD ? 2u : 0u) | (sAsleep ? 1u : 0u));
}

const char* const* g2PetMenuRows(size_t* count) {
  static const char* aliveRows[] = { "<- Back", "Feed", "Play", "Clean", "Medicine", "Sleep", "Stats" };
  static const char* deadRows[]  = { "<- Back", "New Egg" };
  if (sStage == ST_DEAD) {
    if (count) *count = sizeof(deadRows) / sizeof(deadRows[0]);
    return deadRows;
  }
  aliveRows[5] = sAsleep ? "Wake" : "Sleep";
  if (count) *count = sizeof(aliveRows) / sizeof(aliveRows[0]);
  return aliveRows;
}

// ── Render tile ──────────────────────────────────────────────────────────────
EXT_RAM_BSS_ATTR static uint8_t sGrid[PET_TILE_H][PET_TILE_W];

static inline void gSet(int x, int y, int v) {
  if (x >= 0 && x < PET_TILE_W && y >= 0 && y < PET_TILE_H) sGrid[y][x] = (uint8_t)v;
}
static void gFill(int x0, int y0, int w, int h, int v) {
  for (int y = y0; y < y0 + h; y++)
    for (int x = x0; x < x0 + w; x++) gSet(x, y, v);
}
static void blitSprite(const char* const* frame, int ox, int oy, int scale) {
  for (int sy = 0; sy < PET_SPR; sy++) {
    const char* row = frame[sy];
    for (int sx = 0; sx < PET_SPR; sx++) {
      const int idx = petSym(row[sx]);
      if (idx < 0) continue;
      gFill(ox + sx * scale, oy + sy * scale, scale, scale, idx);
    }
  }
}
static void drawEllipse(int cx, int cy, int rx, int ry, int v) {
  if (rx <= 0 || ry <= 0) return;
  const long rx2 = (long)rx * rx, ry2 = (long)ry * ry;
  for (int y = -ry; y <= ry; y++)
    for (int x = -rx; x <= rx; x++)
      if ((long)x * x * ry2 + (long)y * y * rx2 <= rx2 * ry2) gSet(cx + x, cy + y, v);
}
static void drawHeart(int ox, int oy, int v) {
  for (int y = 0; y < 5; y++)
    for (int x = 0; x < 5; x++)
      if (HEART5[y][x] == '#') gSet(ox + x, oy + y, v);
}
static void drawZ(int x, int y, int v) {
  gFill(x, y, 3, 1, v); gSet(x + 1, y + 1, v); gFill(x, y + 2, 3, 1, v);
}
static void drawText(const char* s, int x, int y, int v) {   // 3x5 font, 1px gap
  for (; *s; s++) {
    const char* g = petGlyph(*s);
    if (g) for (int r = 0; r < 5; r++) for (int c = 0; c < 3; c++)
      if (g[r * 3 + c] == '#') gSet(x + c, y + r, v);
    x += 4;
  }
}
static void drawStat(char label, int val, int x, int y) {
  char buf[6]; snprintf(buf, sizeof(buf), "%c%d", label, val);
  drawText(buf, x, y, 13);
}

static const char* const* petMoodFrame(void) {
  const bool reacting = (sReaction != PR_NONE) && (millis() < sReactionEnd);
  if (reacting) {
    switch (sReaction) {
      case PR_EAT:   return (sAnimPhase & 1) ? PET_HAPPY : PET_IDLE;
      case PR_HAPPY: case PR_CLEAN: case PR_MED: return PET_HAPPY;
      case PR_TIRED: return PET_SAD;
      default:       return PET_IDLE;
    }
  }
  if (sAsleep) return PET_BLINK;
  if (sSick)   return PET_SAD;
  const bool critical = sHunger < 20 || sHygiene < 20 || sHappy < 15 || sEnergy < 12;
  const bool content  = sHappy > 75 && sHunger > 50 && sEnergy > 40;
  if (critical)                        return PET_SAD;
  if ((sAnimPhase % 12) == 0)          return PET_BLINK;
  if (content && (sAnimPhase % 6) < 3) return PET_HAPPY;
  return PET_IDLE;
}

static void petComposeGrid(void) {
  const bool reacting = (sReaction != PR_NONE) && (millis() < sReactionEnd);
  memset(sGrid, 0, sizeof(sGrid));

  // Night sky (local hours 21:00–05:59) — crescent moon + a few stars.
  if (Clock::isSynced()) {
    const time_t t = (time_t)Clock::epochSeconds();
    struct tm lt; localtime_r(&t, &lt);
    if (lt.tm_hour >= 21 || lt.tm_hour < 6) {
      drawEllipse(66, 12, 6, 6, 4); drawEllipse(69, 11, 5, 5, 0);   // crescent
      gSet(10, 8, 12); gSet(22, 6, 10); gSet(52, 22, 12); gSet(16, 20, 10);
    }
  }

  const int scale = (sStage == ST_BABY) ? 3 : 4;
  const int ox = (PET_TILE_W - PET_SPR * scale) / 2;
  int bob = sAsleep ? BOB_SLEEP[sAnimPhase % 8] : BOB[sAnimPhase % 8];
  if (sStage == ST_DEAD) bob = BOB_GHOST[sAnimPhase % 8];
  const int oy = (PET_BASE_Y - PET_SPR * scale) + bob;

  // Ground shadow (skip for the floating ghost).
  if (sStage != ST_DEAD) {
    const int base = (sStage == ST_BABY) ? 12 : 18;
    drawEllipse(PET_TILE_W / 2, PET_BASE_Y + 3, base + bob * 2, 3, 3);
  }

  // Body per stage.
  if (sStage == ST_EGG) {
    const bool hatching = (sAgeMin + 1 >= HATCH_MIN);
    const int wob = ((sAnimPhase / 3) & 1) ? (hatching ? 2 : 1) : (hatching ? -2 : -1);
    blitSprite(hatching ? PET_EGG_CRACK : PET_EGG, ox + wob, oy, scale);
  } else if (sStage == ST_DEAD) {
    blitSprite(PET_GHOST, ox, oy, scale);
  } else {
    blitSprite(petMoodFrame(), ox, oy, scale);
    // Stage accessories drawn over the shared body.
    const int headTop = oy + 2 * scale;
    if (sStage == ST_BABY) {                       // sprout
      const int cx = PET_TILE_W / 2;
      gFill(cx, oy - 4, 1, 5, 0); gSet(cx - 2, oy - 3, 10); gSet(cx + 2, oy - 4, 10);
    } else if (sStage == ST_ADULT_A) {             // antennae + sparkle
      const int lx = ox + 5 * scale, rx = ox + 10 * scale;
      gFill(lx, oy - 7, 2, 7, 0); gFill(rx, oy - 7, 2, 7, 0);
      gSet(lx, oy - 8, 15); gSet(rx + 1, oy - 8, 15);
      if (sAnimPhase & 1) { gSet(ox - 4, headTop, 15); gSet(ox - 5, headTop + 1, 12); gSet(ox - 3, headTop + 1, 12); }
    } else if (sStage == ST_ADULT_B) {             // grumpy brow
      const int by = oy + 6 * scale;
      for (int i = 0; i < 3; i++) { gSet(ox + 5 * scale + i, by - i, 0); gSet(ox + 9 * scale + i, by - (2 - i), 0); }
    }
    // Sickness: droplets + a medical cross by the head.
    if (sSick) {
      const int hx = ox + PET_SPR * scale + 1;
      gSet(hx, oy + 3, 6); gSet(hx + 1, oy + 6, 6); gSet(hx, oy + 9, 6);
      gFill(hx, oy - 2, 3, 1, 15); gFill(hx + 1, oy - 3, 1, 3, 15);
    }
  }

  // Poop pile (needs cleaning) — bottom-left, with a lazy fly.
  if (petHatched() && sPoop) {
    gFill(10, 66, 6, 2, 5); gFill(11, 64, 4, 2, 6); gFill(12, 63, 2, 1, 7);
    if (sAnimPhase & 1) gSet(19, 61, 13);
  }

  // Reaction / mood overlays.
  const int cx = PET_TILE_W / 2;
  if (sAsleep) {
    drawZ(52, 6, 13); drawZ(56, 12, 13); drawZ(60, 18, 13);
  } else if (reacting && (sReaction == PR_HAPPY || sReaction == PR_EAT)) {
    if (sAnimPhase & 1) drawHeart(cx - 2, 2, 14);
    if (sReaction == PR_EAT) { const int fy = oy + 6 * scale; gFill(cx - 1, fy, 3, 3, 14); }  // morsel
  } else if (reacting && sReaction == PR_CLEAN) {                                             // bath bubbles
    const int b[5][2] = { {14,20},{66,22},{18,58},{62,56},{cx,3} };
    for (int i = 0; i < 5; i++) if ((sAnimPhase + i) & 1) drawEllipse(b[i][0], b[i][1], 2, 2, 15);
  } else if (reacting && sReaction == PR_MED) {                                               // heal sparkle + cross
    gFill(cx - 1, 3, 3, 1, 15); gFill(cx, 2, 1, 3, 15);
  } else if (petHatched()) {
    const bool critical = sHunger < 20 || sHygiene < 20 || sHappy < 15 || sEnergy < 12;
    if ((critical || sSick) && (sAnimPhase & 1)) { gFill(cx, 2, 2, 5, 15); gFill(cx, 8, 2, 2, 15); }
  }

  // Evolution burst — an expanding sparkle ring for ~1.5 s after evolving.
  if (sEvolveMs && (millis() - sEvolveMs) < 1500) {
    const int r = 6 + (int)((millis() - sEvolveMs) / 90);
    for (int a = 0; a < 8; a++) {
      const int dx[8] = { 0, r, 0, -r, r, r, -r, -r };
      const int dy[8] = { -r, 0, r, 0, -r, r, r, -r };
      gSet(cx + dx[a] / 1, (PET_BASE_Y - 24) + dy[a], 15);
    }
  }

  // On-demand numeric stats (toggle) — dark backing so it reads over the body.
  if (sStatsShown) {
    gFill(1, 44, 22, 28, 2);
    drawStat('H', (int)sHunger,  2, 46);
    drawStat('P', (int)sHappy,   2, 53);
    drawStat('E', (int)sEnergy,  2, 60);
    drawStat('C', (int)sHygiene, 2, 67);
  }
}

static size_t petPackBmp(uint8_t* out, size_t cap) {
  const uint32_t w = PET_TILE_W, h = PET_TILE_H;
  const uint32_t rowStride = ((w * 4 + 31) / 32) * 4;
  const uint32_t pixelSize = rowStride * h;
  const uint32_t headerSize = 14 + 40 + 64;
  const uint32_t total = headerSize + pixelSize;
  if (!out || total > cap) return 0;

  auto wr16 = [](uint8_t* p, uint16_t v) { p[0] = v & 0xff; p[1] = (v >> 8) & 0xff; };
  auto wr32 = [](uint8_t* p, uint32_t v) {
    p[0] = v & 0xff; p[1] = (v >> 8) & 0xff; p[2] = (v >> 16) & 0xff; p[3] = (v >> 24) & 0xff;
  };
  out[0] = 'B'; out[1] = 'M';
  wr32(out + 2, total); wr16(out + 6, 0); wr16(out + 8, 0); wr32(out + 10, headerSize);
  wr32(out + 14, 40); wr32(out + 18, w); wr32(out + 22, (uint32_t)(-(int32_t)h));
  wr16(out + 26, 1); wr16(out + 28, 4); wr32(out + 30, 0); wr32(out + 34, pixelSize);
  wr32(out + 38, 2835); wr32(out + 42, 2835); wr32(out + 46, 16); wr32(out + 50, 0);
  for (int i = 0; i < 16; i++) {
    const uint8_t v = (uint8_t)((i * 255) / 15);
    out[54 + i * 4 + 0] = v; out[54 + i * 4 + 1] = v; out[54 + i * 4 + 2] = v; out[54 + i * 4 + 3] = 0;
  }
  uint8_t* pixels = out + headerSize;
  for (uint32_t y = 0; y < h; y++) {
    uint8_t* dst = pixels + y * rowStride;
    for (uint32_t x = 0; x < w; x += 2)
      dst[x / 2] = (uint8_t)((sGrid[y][x] << 4) | (sGrid[y][x + 1] & 0x0f));
  }
  return total;
}

size_t g2RenderPetBmp(uint8_t* out, size_t cap) {
  petComposeGrid();
  return petPackBmp(out, cap);
}

// ── Small API ────────────────────────────────────────────────────────────────
size_t g2PetBmpCap(void) {
  const uint32_t rowStride = ((PET_TILE_W * 4 + 31) / 32) * 4;
  return (size_t)(14 + 40 + 64) + (size_t)rowStride * PET_TILE_H;
}

void g2PetBuildInfo(char* out, size_t cap) {
  if (!out || !cap) return;
  static const char* kStage[] = { "Egg", "Baby", "Child", "Adult", "Adult", "R.I.P." };
  if (sStage == ST_DEAD) {
    snprintf(out, cap, "Pet — R.I.P. (age %luh, care %d)\nNew Egg to start again",
             (unsigned long)(sAgeMin / 60), sCare);
    return;
  }
  snprintf(out, cap,
           "Pet — %s%s\nHunger %d  Happy %d\nEnergy %d  Clean %d\nAge %luh%s%s",
           kStage[sStage <= ST_DEAD ? sStage : 2], sSick ? " (sick)" : "",
           (int)sHunger, (int)sHappy, (int)sEnergy, (int)sHygiene,
           (unsigned long)(sAgeMin / 60),
           sAsleep ? "  (sleeping)" : "", sPoop ? "  (needs clean)" : "");
}

#endif  // ENABLE_BLUETOOTH && ENABLE_G2_GLASSES
