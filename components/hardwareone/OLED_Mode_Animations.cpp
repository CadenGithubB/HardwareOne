// OLED_Mode_Animations.cpp - Animation system and renderers
// Extracted from OLED_Display.cpp for modularity

#include "OLED_Display.h"
#include "System_BuildConfig.h"

#if ENABLE_OLED_DISPLAY

#include <Adafruit_SSD1306.h>
#include "System_FirstTimeSetup.h"

// External references
extern Adafruit_SSD1306* oledDisplay;
extern bool oledConnected;
extern OLEDAnimationType currentAnimation;
extern unsigned long animationFrame;
extern int animationFPS;

// Boot progress state
extern int bootProgressPercent;
extern String bootProgressLabel;
// gFirstTimeSetupState and gSetupProgressStage are declared in System_FirstTimeSetup.h

// ============================================================================
// Helper Functions
// ============================================================================

// Fast sine approximation for animations (returns -127 to 127)
static int fastSin(int angle) {
  angle = angle % 360;
  if (angle < 0) angle += 360;

  if (angle < 90) {
    return (angle * 127) / 90;
  } else if (angle < 180) {
    return 127 - ((angle - 90) * 127) / 90;
  } else if (angle < 270) {
    return -((angle - 180) * 127) / 90;
  } else {
    return -127 + ((angle - 270) * 127) / 90;
  }
}

// ============================================================================
// Animation Renderers
// ============================================================================

static void renderBounceAnimation() {
  static float ballX = 64;
  static float ballY = 32;
  static float velX = 2.0;
  static float velY = 1.5;
  const int ballRadius = 4;

  ballX += velX;
  ballY += velY;

  if (ballX - ballRadius < 0 || ballX + ballRadius >= SCREEN_WIDTH) {
    velX = -velX;
    ballX = constrain(ballX, ballRadius, SCREEN_WIDTH - ballRadius - 1);
  }
  if (ballY - ballRadius < 0 || ballY + ballRadius >= SCREEN_HEIGHT) {
    velY = -velY;
    ballY = constrain(ballY, ballRadius, SCREEN_HEIGHT - ballRadius - 1);
  }

  oledDisplay->fillCircle((int)ballX, (int)ballY, ballRadius, DISPLAY_COLOR_WHITE);
  oledDisplay->drawCircle((int)(ballX - velX), (int)(ballY - velY), ballRadius - 1, DISPLAY_COLOR_WHITE);
}

static void renderWaveAnimation() {
  const int amplitude = 20;
  const int centerY = SCREEN_HEIGHT / 2;

  for (int x = 0; x < SCREEN_WIDTH; x++) {
    int angle = (x * 360 / SCREEN_WIDTH + animationFrame * 5) % 360;
    int y = centerY + (fastSin(angle) * amplitude) / 127;

    if (y >= 0 && y < SCREEN_HEIGHT) {
      oledDisplay->drawPixel(x, y, DISPLAY_COLOR_WHITE);
      if (y > 0) oledDisplay->drawPixel(x, y - 1, DISPLAY_COLOR_WHITE);
      if (y < SCREEN_HEIGHT - 1) oledDisplay->drawPixel(x, y + 1, DISPLAY_COLOR_WHITE);
    }
  }
}

static void renderSpinnerAnimation() {
  const int centerX = SCREEN_WIDTH / 2;
  const int centerY = SCREEN_HEIGHT / 2;
  const int radius = 25;
  const int numSpokes = 8;

  for (int i = 0; i < numSpokes; i++) {
    int angle = (animationFrame * 10 + i * (360 / numSpokes)) % 360;
    int x = centerX + (fastSin(angle + 90) * radius) / 127;
    int y = centerY + (fastSin(angle) * radius) / 127;
    oledDisplay->drawLine(centerX, centerY, x, y, DISPLAY_COLOR_WHITE);
  }

  oledDisplay->fillCircle(centerX, centerY, 3, DISPLAY_COLOR_WHITE);
}

static void renderMatrixAnimation() {
  static uint8_t columns[128];
  static bool initialized = false;

  if (!initialized) {
    for (int i = 0; i < SCREEN_WIDTH; i++) {
      columns[i] = random(SCREEN_HEIGHT);
    }
    initialized = true;
  }

  if (animationFrame % 2 == 0) {
    for (int x = 0; x < SCREEN_WIDTH; x += 4) {
      columns[x] = (columns[x] + 1) % (SCREEN_HEIGHT + 20);

      if (columns[x] < SCREEN_HEIGHT) {
        int y = columns[x];
        for (int dy = 0; dy < 6 && y + dy < SCREEN_HEIGHT; dy++) {
          oledDisplay->drawPixel(x, y + dy, DISPLAY_COLOR_WHITE);
        }
      }
    }
  }
}

static void renderStarfieldAnimation() {
  static int stars[40][3];
  static bool initialized = false;

  if (!initialized) {
    for (int i = 0; i < 40; i++) {
      stars[i][0] = random(SCREEN_WIDTH);
      stars[i][1] = random(SCREEN_HEIGHT);
      stars[i][2] = random(1, 4);
    }
    initialized = true;
  }

  for (int i = 0; i < 40; i++) {
    stars[i][0] -= stars[i][2];

    if (stars[i][0] < 0) {
      stars[i][0] = SCREEN_WIDTH - 1;
      stars[i][1] = random(SCREEN_HEIGHT);
    }

    int x = stars[i][0];
    int y = stars[i][1];
    oledDisplay->drawPixel(x, y, DISPLAY_COLOR_WHITE);
    if (stars[i][2] > 2) {
      if (x < SCREEN_WIDTH - 1) oledDisplay->drawPixel(x + 1, y, DISPLAY_COLOR_WHITE);
    }
  }
}

static void renderPlasmaAnimation() {
  for (int y = 0; y < SCREEN_HEIGHT; y += 2) {
    for (int x = 0; x < SCREEN_WIDTH; x += 2) {
      int v1 = fastSin((x * 4 + animationFrame * 2) % 360);
      int v2 = fastSin((y * 4 + animationFrame * 3) % 360);
      int plasma = (v1 + v2) / 2;

      if (plasma > 0) {
        oledDisplay->drawPixel(x, y, DISPLAY_COLOR_WHITE);
      }
    }
  }
}

static void renderFireAnimation() {
  static uint8_t fire[128];
  static bool initialized = false;

  if (!initialized) {
    for (int i = 0; i < SCREEN_WIDTH; i++) {
      fire[i] = random(256);
    }
    initialized = true;
  }

  for (int x = 0; x < SCREEN_WIDTH; x++) {
    fire[x] = random(200, 256);
  }

  for (int y = 0; y < SCREEN_HEIGHT; y++) {
    for (int x = 0; x < SCREEN_WIDTH; x++) {
      int heat = fire[x] * (SCREEN_HEIGHT - y) / SCREEN_HEIGHT;

      bool draw = false;
      if (heat > 200) {
        draw = true;
      } else if (heat > 128) {
        draw = ((x + y) % 2 == 0);
      } else if (heat > 64) {
        draw = ((x + y) % 4 == 0);
      }

      if (draw) {
        oledDisplay->drawPixel(x, SCREEN_HEIGHT - 1 - y, DISPLAY_COLOR_WHITE);
      }
    }
  }
}

static void renderGameOfLifeAnimation() {
  static uint8_t grid[64][32];
  static bool initialized = false;

  if (!initialized) {
    for (int y = 0; y < 32; y++) {
      for (int x = 0; x < 64; x++) {
        grid[x][y] = random(2);
      }
    }
    initialized = true;
  }

  if (animationFrame % 10 == 0) {
    uint8_t newGrid[64][32];

    for (int y = 0; y < 32; y++) {
      for (int x = 0; x < 64; x++) {
        int neighbors = 0;
        for (int dy = -1; dy <= 1; dy++) {
          for (int dx = -1; dx <= 1; dx++) {
            if (dx == 0 && dy == 0) continue;
            int nx = (x + dx + 64) % 64;
            int ny = (y + dy + 32) % 32;
            neighbors += grid[nx][ny];
          }
        }

        if (grid[x][y] == 1) {
          newGrid[x][y] = (neighbors == 2 || neighbors == 3) ? 1 : 0;
        } else {
          newGrid[x][y] = (neighbors == 3) ? 1 : 0;
        }
      }
    }

    memcpy(grid, newGrid, sizeof(grid));
  }

  for (int y = 0; y < 32; y++) {
    for (int x = 0; x < 64; x++) {
      if (grid[x][y]) {
        oledDisplay->fillRect(x * 2, y * 2, 2, 2, DISPLAY_COLOR_WHITE);
      }
    }
  }
}

static void renderRadarAnimation() {
  const int centerX = SCREEN_WIDTH / 2;
  const int centerY = SCREEN_HEIGHT / 2;
  const int maxRadius = 30;

  for (int r = 10; r <= maxRadius; r += 10) {
    oledDisplay->drawCircle(centerX, centerY, r, DISPLAY_COLOR_WHITE);
  }

  int angle = (animationFrame * 6) % 360;
  int x = centerX + (fastSin(angle + 90) * maxRadius) / 127;
  int y = centerY + (fastSin(angle) * maxRadius) / 127;
  oledDisplay->drawLine(centerX, centerY, x, y, DISPLAY_COLOR_WHITE);

  static int blips[5][2];
  static bool blipsInit = false;
  if (!blipsInit || animationFrame % 30 == 0) {
    for (int i = 0; i < 5; i++) {
      blips[i][0] = random(-maxRadius, maxRadius);
      blips[i][1] = random(-maxRadius, maxRadius);
    }
    blipsInit = true;
  }

  for (int i = 0; i < 5; i++) {
    int bx = centerX + blips[i][0];
    int by = centerY + blips[i][1];
    if (bx >= 0 && bx < SCREEN_WIDTH && by >= 0 && by < SCREEN_HEIGHT) {
      oledDisplay->fillCircle(bx, by, 2, DISPLAY_COLOR_WHITE);
    }
  }
}

static void renderWaveformAnimation() {
  static int waveform[128];
  static bool initialized = false;

  if (!initialized || animationFrame % 3 == 0) {
    for (int i = 0; i < SCREEN_WIDTH; i++) {
      waveform[i] = random(-20, 20);
    }
    initialized = true;
  }

  oledDisplay->drawLine(0, SCREEN_HEIGHT / 2, SCREEN_WIDTH, SCREEN_HEIGHT / 2, DISPLAY_COLOR_WHITE);

  for (int x = 0; x < SCREEN_WIDTH - 1; x++) {
    int y1 = SCREEN_HEIGHT / 2 + waveform[x];
    int y2 = SCREEN_HEIGHT / 2 + waveform[x + 1];
    oledDisplay->drawLine(x, y1, x + 1, y2, DISPLAY_COLOR_WHITE);
  }
}

static void renderScrollTestAnimation() {
  // Smooth horizontal scrolling test to verify frame rate
  const int barWidth = 40;
  const int barHeight = 20;
  int barX = (animationFrame * 2) % (SCREEN_WIDTH + barWidth) - barWidth;
  int barY = SCREEN_HEIGHT / 2 - barHeight / 2;
  
  // Draw scrolling bar
  oledDisplay->fillRect(barX, barY, barWidth, barHeight, DISPLAY_COLOR_WHITE);
  
  // Draw reference grid
  for (int x = 0; x < SCREEN_WIDTH; x += 16) {
    oledDisplay->drawLine(x, 0, x, SCREEN_HEIGHT - 1, DISPLAY_COLOR_WHITE);
  }
  for (int y = 0; y < SCREEN_HEIGHT; y += 16) {
    oledDisplay->drawLine(0, y, SCREEN_WIDTH - 1, y, DISPLAY_COLOR_WHITE);
  }
  
  // Show FPS at top
  oledDisplay->setTextSize(1);
  oledDisplay->setCursor(2, 2);
  oledDisplay->print("FPS: ");
  oledDisplay->print(animationFPS);
  
  // Show frame count
  oledDisplay->setCursor(2, 12);
  oledDisplay->print("Frame: ");
  oledDisplay->print(animationFrame % 1000);
  
  // Instructions at bottom
  oledDisplay->setCursor(0, SCREEN_HEIGHT - 8);
  oledDisplay->print("Smooth = Good");
}

// ============================================================================
// Boot Progress Animation
// ============================================================================

static void drawProgressBar(int percent) {
  const int barX = 10;
  const int barY = 35;
  const int barWidth = SCREEN_WIDTH - 20;
  const int barHeight = 12;

  oledDisplay->drawRect(barX, barY, barWidth, barHeight, DISPLAY_COLOR_WHITE);

  int fillWidth = (barWidth - 4) * percent / 100;
  if (fillWidth > 0) {
    oledDisplay->fillRect(barX + 2, barY + 2, fillWidth, barHeight - 4, DISPLAY_COLOR_WHITE);
  }

  oledDisplay->setCursor(barX + barWidth / 2 - 12, barY + barHeight + 6);
  oledDisplay->print(percent);
  oledDisplay->print("%");
}

static void showFirstTimeSetupPrompt() {
  oledDisplay->println("First-Time Setup");
  oledDisplay->println("Required!");
  oledDisplay->println();
  oledDisplay->println("Please open the");
  oledDisplay->println("Serial Console");
  oledDisplay->println("(115200 baud)");
}

static void showFirstTimeSetupProgress() {
  oledDisplay->println("Setup in Progress");
  oledDisplay->println();
  
  const char* message = getSetupProgressMessage(gSetupProgressStage);
  oledDisplay->println(message);
  
  // Show progress bar
  int progress = ((gSetupProgressStage + 1) * 100) / 5;
  drawProgressBar(progress);
}

static void showSetupCompleteMessage() {
  oledDisplay->println("Setup Complete!");
  oledDisplay->println();
  oledDisplay->println("Starting WiFi...");
  oledDisplay->println("Please wait");
}

static void showNormalBootProgress() {
  if (bootProgressLabel.length() > 0) {
    oledDisplay->println(bootProgressLabel);
  } else {
    oledDisplay->println("Booting...");
  }
  oledDisplay->println();

  const int barX = 10;
  const int barY = 35;
  const int barWidth = SCREEN_WIDTH - 20;
  const int barHeight = 12;

  oledDisplay->drawRect(barX, barY, barWidth, barHeight, DISPLAY_COLOR_WHITE);

  int fillWidth = (barWidth - 4) * bootProgressPercent / 100;
  if (fillWidth > 0) {
    oledDisplay->fillRect(barX + 2, barY + 2, fillWidth, barHeight - 4, DISPLAY_COLOR_WHITE);
  }

  oledDisplay->setCursor(barX + barWidth / 2 - 12, barY + barHeight + 6);
  oledDisplay->print(bootProgressPercent);
  oledDisplay->print("%");
}

static void renderBootProgressAnimation() {
  FirstTimeSetupState state = gFirstTimeSetupState;

  oledDisplay->setTextSize(1);
  oledDisplay->setCursor(0, 0);
  oledDisplay->println("HardwareOne v2.1");
  oledDisplay->println();

  switch (state) {
    case SETUP_REQUIRED:
      showFirstTimeSetupPrompt();
      break;
    case SETUP_IN_PROGRESS:
      showFirstTimeSetupProgress();
      break;
    case SETUP_COMPLETE:
      showSetupCompleteMessage();
      break;
    case SETUP_NOT_NEEDED:
    default:
      showNormalBootProgress();
      break;
  }
}

// ============================================================================
// Animation Registry
// ============================================================================

const OLEDAnimation gAnimationRegistry[] = {
  { "bounce", ANIM_BOUNCE, renderBounceAnimation, "Bouncing ball" },
  { "wave", ANIM_WAVE, renderWaveAnimation, "Sine wave" },
  { "spinner", ANIM_SPINNER, renderSpinnerAnimation, "Rotating spinner" },
  { "matrix", ANIM_MATRIX, renderMatrixAnimation, "Matrix rain effect" },
  { "starfield", ANIM_STARFIELD, renderStarfieldAnimation, "Moving starfield" },
  { "plasma", ANIM_PLASMA, renderPlasmaAnimation, "Plasma effect" },
  { "fire", ANIM_FIRE, renderFireAnimation, "Fire simulation" },
  { "life", ANIM_GAME_OF_LIFE, renderGameOfLifeAnimation, "Conway's Game of Life" },
  { "radar", ANIM_RADAR, renderRadarAnimation, "Radar sweep" },
  { "waveform", ANIM_WAVEFORM, renderWaveformAnimation, "Audio waveform" },
  { "scrolltest", ANIM_SCROLLTEST, renderScrollTestAnimation, "Smooth scroll test" },
  { "bootprogress", ANIM_BOOT_PROGRESS, renderBootProgressAnimation, "Boot progress bar" }
};
const int gAnimationCount = sizeof(gAnimationRegistry) / sizeof(OLEDAnimation);

// ============================================================================
// Display Animation Function
// ============================================================================

void displayAnimation() {
  if (!oledDisplay || !oledConnected) return;
  
  for (int i = 0; i < gAnimationCount; i++) {
    if (gAnimationRegistry[i].type == currentAnimation) {
      gAnimationRegistry[i].renderFunc();
      break;
    }
  }

  // Show compact skip hint in bottom left corner only (animations use full screen)
  if (currentAnimation != ANIM_BOOT_PROGRESS) {
    const int hintWidth = 42;
    const int hintY = SCREEN_HEIGHT - 9;
    
    oledDisplay->fillRect(0, hintY, hintWidth, 9, DISPLAY_COLOR_BLACK);
    oledDisplay->drawRect(0, hintY, hintWidth, 9, DISPLAY_COLOR_WHITE);
    
    oledDisplay->setTextSize(1);
    oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
    oledDisplay->setCursor(2, hintY + 1);
    oledDisplay->print("B:Back");
  }
}

#endif // ENABLE_OLED_DISPLAY
