// i2csensor-seesaw-oled.h - Seesaw Gamepad OLED display functions
// Include this at the end of i2csensor-seesaw.cpp
#ifndef I2CSENSOR_SEESAW_OLED_H
#define I2CSENSOR_SEESAW_OLED_H

#include "OLED_Display.h"
#include <Adafruit_SSD1306.h>

// External OLED display pointer
extern Adafruit_SSD1306* oledDisplay;

// Button bit definitions are now in Sensor_Gamepad_Seesaw.h
#define GAMEPAD_BUTTON_SEL   0  // Only SEL is not in header

// Display gamepad state visualization
static void displayGamepadVisual() {
  if (!oledDisplay) return;
  
  oledDisplay->setTextSize(1);
  oledDisplay->println("=== GAMEPAD ===");
  
  if (!gamepadEnabled || !gamepadConnected) {
    oledDisplay->println();
    oledDisplay->println("Gamepad not active");
    oledDisplay->println();
    oledDisplay->println("Use 'gamepadstart'");
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
  
  // Draw joystick position (left side)
  // Joystick area: 28x28 box at position (5, 15) - constrained to content area
  const int joyBoxX = 5, joyBoxY = 15, joyBoxSize = 28;
  oledDisplay->drawRect(joyBoxX, joyBoxY, joyBoxSize, joyBoxSize, DISPLAY_COLOR_WHITE);
  
  // Map joystick (0-1023) to box position
  int dotX = map(joyX, 0, 1023, joyBoxX + 2, joyBoxX + joyBoxSize - 4);
  // Invert Y so physical UP renders at the top of the box
  int dotY = map(joyY, 1023, 0, joyBoxY + 2, joyBoxY + joyBoxSize - 4);
  oledDisplay->fillCircle(dotX, dotY, 3, DISPLAY_COLOR_WHITE);
  
  // Draw joystick values below - constrained to content area (max Y = 53)
  oledDisplay->setCursor(0, 46);
  oledDisplay->setTextSize(1);
  oledDisplay->printf("X:%4d Y:%4d", joyX, joyY);
  
  // Draw buttons (right side) - visual representation
  // Button layout similar to SNES controller, but with X/Y swapped on screen:
  //       X
  //    Y     A
  //       B
  //  [SEL] [START]
  
  const int btnBaseX = 85, btnBaseY = 20;
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
  
  // SELECT and START as small rectangles at bottom - constrained to content area
  const int metaBtnY = 46;
  
  // START - logical INPUT_BUTTON_START - LEFT
  if (INPUT_CHECK(pressed, INPUT_BUTTON_START))
    oledDisplay->fillRoundRect(btnBaseX - 5, metaBtnY, 20, 8, 2, DISPLAY_COLOR_WHITE);
  else
    oledDisplay->drawRoundRect(btnBaseX - 5, metaBtnY, 20, 8, 2, DISPLAY_COLOR_WHITE);
  oledDisplay->setCursor(btnBaseX - 2, metaBtnY + 1);
  oledDisplay->print("STRT");
  
  // SELECT - bit 0 - RIGHT
  if (pressed & (1 << GAMEPAD_BUTTON_SEL))
    oledDisplay->fillRoundRect(btnBaseX + 18, metaBtnY, 24, 8, 2, DISPLAY_COLOR_WHITE);
  else
    oledDisplay->drawRoundRect(btnBaseX + 18, metaBtnY, 24, 8, 2, DISPLAY_COLOR_WHITE);
  oledDisplay->setCursor(btnBaseX + 20, metaBtnY + 1);
  oledDisplay->print("SEL");
}

// Availability check for Gamepad OLED mode
static bool gamepadOLEDModeAvailable(String* outReason) {
  return true;  // Always show in menu, display function handles inactive state
}

// Input handler for Gamepad OLED mode - X button toggles gamepad
static bool gamepadInputHandler(int deltaX, int deltaY, uint32_t newlyPressed) {
  if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_X)) {
    extern bool enqueueSensorStart(SensorType sensor);
    extern bool isInQueue(SensorType sensor);
    
    if (gamepadEnabled && gamepadConnected) {
      Serial.println("[GAMEPAD] X button: Stopping gamepad...");
      gamepadEnabled = false;
    } else if (!isInQueue(SENSOR_GAMEPAD)) {
      Serial.println("[GAMEPAD] X button: Starting gamepad...");
      enqueueSensorStart(SENSOR_GAMEPAD);
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
