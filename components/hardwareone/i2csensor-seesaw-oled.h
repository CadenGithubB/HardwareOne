// i2csensor-seesaw-oled.h - Seesaw Gamepad OLED display functions
// Include this at the end of i2csensor-seesaw.cpp
#ifndef I2CSENSOR_SEESAW_OLED_H
#define I2CSENSOR_SEESAW_OLED_H

#include "OLED_Display.h"
#include "OLED_Utils.h"
#include "HAL_Display.h"  // For gDisplay and oledDisplay macro alias

// Button bit definitions are now in Sensor_Gamepad_Seesaw.h
#define GAMEPAD_BUTTON_SEL   0  // Only SEL is not in header

// Display gamepad state visualization
static void displayGamepadVisual() {
  if (!oledDisplay) return;
  
  oledDisplay->setTextSize(1);
  
  if (!gamepadEnabled || !gamepadConnected) {
    oledDisplay->setCursor(0, OLED_CONTENT_START_Y);
    oledDisplay->println("Gamepad not active");
    oledDisplay->println();
    oledDisplay->println("Use 'opengamepad'");
    oledDisplay->println("to enable");
    return;
  }
  
  // Read from cache
  int joyX = 512, joyY = 512;
  uint32_t buttons = 0xFFFFFFFF;  // All unpressed (active low)
  bool dataValid = false;
  
  if (gControlCache.mutex && xSemaphoreTake(gControlCache.mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
    if (gControlCache.gamepadDataValid) {
      joyX = gControlCache.gamepadX;
      joyY = gControlCache.gamepadY;
      buttons = gControlCache.gamepadButtons;
      dataValid = true;
    }
    xSemaphoreGive(gControlCache.mutex);
  }
  
  if (!dataValid) {
    oledDisplay->println();
    oledDisplay->println("Waiting for data...");
    return;
  }
  
  // Invert buttons for active-high logic (0 = not pressed, 1 = pressed)
  uint32_t pressed = ~buttons;
  
  // Center layout vertically in content area
  // Content area: OLED_CONTENT_START_Y to OLED_CONTENT_START_Y + OLED_CONTENT_HEIGHT (43px)
  // Total layout height: ~40px (text 8 + gap 2 + joystick/buttons 28 + meta buttons)
  const int layoutHeight = 40;
  const int layoutTopY = OLED_CONTENT_START_Y + (OLED_CONTENT_HEIGHT - layoutHeight) / 2;
  
  // X:nnn Y:nnn text at top of layout
  oledDisplay->setCursor(0, layoutTopY);
  oledDisplay->setTextSize(1);
  oledDisplay->printf("X:%4d Y:%4d", joyX, joyY);
  
  // Draw joystick position (left side)
  const int joyBoxX = 5, joyBoxY = layoutTopY + 10, joyBoxSize = 28;
  oledDisplay->drawRect(joyBoxX, joyBoxY, joyBoxSize, joyBoxSize, DISPLAY_COLOR_WHITE);
  
  // Map joystick (0-1023) to box position
  int dotX = map(joyX, 0, 1023, joyBoxX + 2, joyBoxX + joyBoxSize - 4);
  // Invert Y so physical UP renders at the top of the box
  int dotY = map(joyY, 1023, 0, joyBoxY + 2, joyBoxY + joyBoxSize - 4);
  oledDisplay->fillCircle(dotX, dotY, 3, DISPLAY_COLOR_WHITE);
  
  // Draw buttons (right side) - visual representation
  // Button layout similar to SNES controller:
  //       X
  //    Y     A
  //       B
  //  [SELECT] [START]
  
  const int btnBaseX = 85, btnBaseY = layoutTopY + 5;
  const int btnR = 5;  // Button radius
  
  // X button (top) - logical INPUT_BUTTON_X
  if (INPUT_CHECK(pressed, INPUT_BUTTON_X))
    oledDisplay->fillCircle(btnBaseX + 15, btnBaseY, btnR, DISPLAY_COLOR_WHITE);
  else
    oledDisplay->drawCircle(btnBaseX + 15, btnBaseY, btnR, DISPLAY_COLOR_WHITE);
  oledDisplay->setCursor(btnBaseX + 13, btnBaseY - 3);
  oledDisplay->print("X");
  
  // Y button (left) - logical INPUT_BUTTON_Y
  if (INPUT_CHECK(pressed, INPUT_BUTTON_Y))
    oledDisplay->fillCircle(btnBaseX, btnBaseY + 12, btnR, DISPLAY_COLOR_WHITE);
  else
    oledDisplay->drawCircle(btnBaseX, btnBaseY + 12, btnR, DISPLAY_COLOR_WHITE);
  oledDisplay->setCursor(btnBaseX - 2, btnBaseY + 9);
  oledDisplay->print("Y");
  
  // A button (right) - logical INPUT_BUTTON_A
  if (INPUT_CHECK(pressed, INPUT_BUTTON_A))
    oledDisplay->fillCircle(btnBaseX + 30, btnBaseY + 12, btnR, DISPLAY_COLOR_WHITE);
  else
    oledDisplay->drawCircle(btnBaseX + 30, btnBaseY + 12, btnR, DISPLAY_COLOR_WHITE);
  oledDisplay->setCursor(btnBaseX + 28, btnBaseY + 9);
  oledDisplay->print("A");
  
  // B button (bottom) - logical INPUT_BUTTON_B
  if (INPUT_CHECK(pressed, INPUT_BUTTON_B))
    oledDisplay->fillCircle(btnBaseX + 15, btnBaseY + 24, btnR, DISPLAY_COLOR_WHITE);
  else
    oledDisplay->drawCircle(btnBaseX + 15, btnBaseY + 24, btnR, DISPLAY_COLOR_WHITE);
  oledDisplay->setCursor(btnBaseX + 13, btnBaseY + 21);
  oledDisplay->print("B");
  
  // SELECT and START moved into the middle gap between joystick and ABXY
  const int metaBtnX = 38;
  const int metaBtnY = layoutTopY + 30;
  const int metaBtnW = 40;
  const int metaBtnH = 10;
  const int metaBtnR = 2;
  
  // SELECT - bit 0
  if (pressed & (1 << GAMEPAD_BUTTON_SEL))
    oledDisplay->fillRoundRect(metaBtnX, metaBtnY - 14, metaBtnW, metaBtnH, metaBtnR, DISPLAY_COLOR_WHITE);
  else
    oledDisplay->drawRoundRect(metaBtnX, metaBtnY - 14, metaBtnW, metaBtnH, metaBtnR, DISPLAY_COLOR_WHITE);
  oledDisplay->setCursor(metaBtnX + 3, metaBtnY - 12);
  oledDisplay->print("SELECT");
  
  // START - logical INPUT_BUTTON_START
  if (INPUT_CHECK(pressed, INPUT_BUTTON_START))
    oledDisplay->fillRoundRect(metaBtnX, metaBtnY, metaBtnW, metaBtnH, metaBtnR, DISPLAY_COLOR_WHITE);
  else
    oledDisplay->drawRoundRect(metaBtnX, metaBtnY, metaBtnW, metaBtnH, metaBtnR, DISPLAY_COLOR_WHITE);
  oledDisplay->setCursor(metaBtnX + 8, metaBtnY + 2);
  oledDisplay->print("START");
}

// Availability check for Gamepad OLED mode
static bool gamepadOLEDModeAvailable(String* outReason) {
  return true;  // Always show in menu, display function handles inactive state
}

static void gamepadToggleConfirmed(void* userData) {
  (void)userData;
  // enqueueDeviceStart, isInQueue provided by System_I2C.h (included in parent .cpp)

  if (gamepadEnabled && gamepadConnected) {
    Serial.println("[GAMEPAD] Confirmed: Stopping gamepad...");
    gamepadEnabled = false;
  } else if (!isInQueue(I2C_DEVICE_GAMEPAD)) {
    Serial.println("[GAMEPAD] Confirmed: Starting gamepad...");
    enqueueDeviceStart(I2C_DEVICE_GAMEPAD);
  }
}

// Input handler for Gamepad OLED mode - X button toggles gamepad
static bool gamepadInputHandler(int deltaX, int deltaY, uint32_t newlyPressed) {
  if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_X)) {
    if (gamepadEnabled && gamepadConnected) {
      oledConfirmRequest("Close gamepad?", "This disables input", gamepadToggleConfirmed, nullptr, false);
    } else {
      oledConfirmRequest("Open gamepad?", nullptr, gamepadToggleConfirmed, nullptr);
    }
    return true;
  }
  return false;
}

// Gamepad OLED mode entry
static const OLEDModeEntry gamepadOLEDModes[] = {
  {
    OLED_GAMEPAD_VISUAL,     // mode enum
    "Gamepad",               // menu name
    "gamepad",               // icon name
    displayGamepadVisual,    // displayFunc
    gamepadOLEDModeAvailable, // availFunc
    gamepadInputHandler,     // inputFunc - X toggles gamepad
    true,                    // showInMenu
    25                       // menuOrder
  }
};

// Auto-register Gamepad OLED mode
REGISTER_OLED_MODE_MODULE(gamepadOLEDModes, sizeof(gamepadOLEDModes) / sizeof(gamepadOLEDModes[0]), "Gamepad");

#endif // I2CSENSOR_SEESAW_OLED_H
