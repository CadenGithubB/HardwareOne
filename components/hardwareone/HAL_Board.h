// HAL_Board.h - board facts that belong to no single peripheral: shared power
// rails and the antenna switch. The pins are the board's (System_BuildConfig.h);
// this layer is the only code that drives them, and every call is a no-op on a
// board without the part, so callers need no build condition.
//
// A rail or enable line that feeds one peripheral stays with that peripheral's
// HAL (the microphone, camera and SD card power pins; the SPI card's chip
// select). HAL_Display.cpp still drives the bus-1 rail itself when it removes
// and restores the panel's power around sleep.
//
// Threading: boot and the bus owner call these; none is safe from an ISR.
#ifndef HAL_BOARD_H
#define HAL_BOARD_H

// The board's switched peripheral rail (NEOPIXEL_POWER_PIN). On some boards
// the pixel's power pin also feeds the STEMMA QT connector (Feather V2), so
// boot asserts it before the I2C buses start, whether or not the LED is
// compiled in. Waits 10 ms for the rail. Asserting a rail twice is fine.
void boardPowerRailInit();

// The switchable supply of I2C bus 1 (I2C2_POWER_PIN; the FeatherS3's LDO2).
// boardI2c2RailOn drives it high and returns the GPIO, or -1 on a board
// without one; the caller owns the settling delay before it clocks the bus.
bool boardHasI2c2Rail();
int boardI2c2RailOn();

// The software RF switch (WIFI_ANTENNA_SWITCH_PIN; the FeatherS3[D]).
// boardAntennaSelectExternal drives the switch to the external (u.FL)
// antenna and returns the GPIO, or -1 without a switch. There is no call for
// the on-board antenna: the board's pull-down selects it, and leaving the pin
// alone keeps a board of the same variant without the switch untouched.
// Call it before any radio starts.
bool boardHasAntennaSwitch();
int boardAntennaSelectExternal();

#endif  // HAL_BOARD_H
